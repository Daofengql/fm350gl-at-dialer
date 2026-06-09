#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

std::atomic_bool g_running{true};
std::mutex g_log_mutex;

struct Options {
    bool scan_only = false;
    bool detailed_scan = false;
    bool list_interfaces = false;
    bool auto_dial = false;
    bool auto_select = false;
    bool dry_run = false;
    std::string port;
    int baudrate = 115200;
    std::string apn = "ctnet";
    std::string interface_name;
    std::string mac_address;
};

struct PortResult {
    std::string device;
    int baudrate = 0;
    std::string info;
};

struct NetworkInterface {
    std::string name;
    std::string type;
    std::string mac;
    std::string ipv4;
};

std::string now() {
    const auto current = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(current);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

void log(const std::string& message, const std::string& level = "INFO") {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << "[" << now() << "] [" << level << "] " << message << '\n';
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string normalize_mac(std::string mac) {
    std::string clean;
    for (char ch : mac) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            clean.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return clean;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string quote_arg(const std::string& value) {
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += '"';
    return quoted;
}

int run_command(const std::string& command, bool dry_run) {
    if (dry_run) {
        log("dry-run: " + command);
        return 0;
    }
    log("run: " + command, "DEBUG");
    return std::system(command.c_str());
}

std::optional<std::string> first_ipv4(const std::string& text) {
    static const std::regex ipv4_re(R"((\d{1,3}(?:\.\d{1,3}){3}))");
    std::smatch match;
    if (std::regex_search(text, match, ipv4_re)) {
        return match.str(1);
    }
    return std::nullopt;
}

std::vector<std::string> all_ipv4(const std::string& text) {
    static const std::regex ipv4_re(R"((\d{1,3}(?:\.\d{1,3}){3}))");
    std::vector<std::string> values;
    for (std::sregex_iterator it(text.begin(), text.end(), ipv4_re), end; it != end; ++it) {
        values.push_back((*it).str(1));
    }
    return values;
}

#ifdef _WIN32
std::string win_error_message(DWORD code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string message = size && buffer ? buffer : "unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    return trim(message);
}

std::string wide_to_utf8(const wchar_t* value) {
    if (!value) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    return result;
}
#endif

class SerialPort {
public:
    SerialPort(const std::string& device, int baudrate, int timeout_ms) {
        open(device, baudrate, timeout_ms);
    }

    ~SerialPort() {
        close();
    }

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    void flush() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
        }
#else
        if (fd_ >= 0) {
            tcflush(fd_, TCIOFLUSH);
        }
#endif
    }

    void write_text(const std::string& text) {
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
            throw std::runtime_error("serial write failed: " + win_error_message(GetLastError()));
        }
#else
        const ssize_t written = ::write(fd_, text.data(), text.size());
        if (written < 0) {
            throw std::runtime_error("serial write failed");
        }
#endif
    }

    std::string read_text(size_t max_bytes = 1024) {
        std::string result(max_bytes, '\0');
#ifdef _WIN32
        DWORD read = 0;
        if (!ReadFile(handle_, result.data(), static_cast<DWORD>(result.size()), &read, nullptr)) {
            throw std::runtime_error("serial read failed: " + win_error_message(GetLastError()));
        }
        result.resize(read);
#else
        const ssize_t read = ::read(fd_, result.data(), result.size());
        if (read < 0) {
            throw std::runtime_error("serial read failed");
        }
        result.resize(static_cast<size_t>(read));
#endif
        return result;
    }

private:
    void open(const std::string& device, int baudrate, int timeout_ms) {
#ifdef _WIN32
        std::string path = device;
        if (starts_with(to_lower(path), "com")) {
            path = "\\\\.\\" + path;
        }
        handle_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot open " + device + ": " + win_error_message(GetLastError()));
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) {
            throw std::runtime_error("GetCommState failed: " + win_error_message(GetLastError()));
        }
        dcb.BaudRate = static_cast<DWORD>(baudrate);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(handle_, &dcb)) {
            throw std::runtime_error("SetCommState failed: " + win_error_message(GetLastError()));
        }

        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout_ms);
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.WriteTotalTimeoutConstant = static_cast<DWORD>(timeout_ms);
        timeouts.WriteTotalTimeoutMultiplier = 0;
        SetCommTimeouts(handle_, &timeouts);
#else
        fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
        if (fd_ < 0) {
            throw std::runtime_error("cannot open " + device);
        }

        termios tty{};
        if (tcgetattr(fd_, &tty) != 0) {
            throw std::runtime_error("tcgetattr failed");
        }

        cfmakeraw(&tty);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= CS8;
        tty.c_cflag &= ~PARENB;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = static_cast<cc_t>(std::max(1, timeout_ms / 100));

        const speed_t speed = baud_to_constant(baudrate);
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);

        if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
            throw std::runtime_error("tcsetattr failed");
        }
#endif
    }

#ifndef _WIN32
    static speed_t baud_to_constant(int baudrate) {
        switch (baudrate) {
            case 9600: return B9600;
            case 19200: return B19200;
            case 38400: return B38400;
            case 57600: return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
#ifdef B460800
            case 460800: return B460800;
#endif
#ifdef B921600
            case 921600: return B921600;
#endif
            default: return B115200;
        }
    }
#endif

    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

std::string send_at(SerialPort& serial, const std::string& command, int wait_ms = 500, size_t max_bytes = 1024) {
    serial.flush();
    serial.write_text(command + "\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    return serial.read_text(max_bytes);
}

std::vector<std::string> scan_serial_ports() {
    std::vector<std::string> ports;
#ifdef _WIN32
    std::array<char, 512> target{};
    for (int i = 1; i <= 256; ++i) {
        const std::string name = "COM" + std::to_string(i);
        if (QueryDosDeviceA(name.c_str(), target.data(), static_cast<DWORD>(target.size())) != 0) {
            ports.push_back(name);
        }
    }
#else
    const std::vector<std::string> prefixes = {"ttyUSB", "ttyACM"};
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        for (const auto& prefix : prefixes) {
            if (starts_with(name, prefix)) {
                ports.push_back("/dev/" + name);
            }
        }
    }
    std::sort(ports.begin(), ports.end());
#endif
    return ports;
}

void display_ports(const std::vector<std::string>& ports) {
    std::cout << "\n============================================================\n";
    std::cout << "Serial ports\n";
    std::cout << "============================================================\n";
    if (ports.empty()) {
        std::cout << "No serial ports found.\n";
    } else {
        for (size_t i = 0; i < ports.size(); ++i) {
            std::cout << "[" << (i + 1) << "] " << ports[i] << '\n';
        }
    }
    std::cout << "============================================================\n\n";
}

std::optional<PortResult> test_port(const std::string& port) {
    const std::vector<int> baudrates = {115200, 9600, 19200, 38400, 57600, 230400, 460800, 921600};
    for (int baudrate : baudrates) {
        if (!g_running) {
            return std::nullopt;
        }
        try {
            SerialPort serial(port, baudrate, 1000);
            const std::string response = send_at(serial, "AT", 100, 256);
            if (response.find("OK") != std::string::npos) {
                const std::string info = trim(send_at(serial, "ATI", 300, 1024));
                log("usable AT port: " + port + " @ " + std::to_string(baudrate));
                return PortResult{port, baudrate, info};
            }
        } catch (const std::exception&) {
        }
    }
    log("no AT response from " + port, "DEBUG");
    return std::nullopt;
}

std::vector<PortResult> detailed_scan(const std::vector<std::string>& ports) {
    std::vector<std::future<std::optional<PortResult>>> futures;
    for (const auto& port : ports) {
        auto promise = std::make_shared<std::promise<std::optional<PortResult>>>();
        futures.emplace_back(promise->get_future());
        std::thread([promise, port]() {
            try {
                promise->set_value(test_port(port));
            } catch (...) {
                promise->set_value(std::nullopt);
            }
        }).detach();
    }

    std::vector<PortResult> results;
    std::vector<bool> consumed(futures.size(), false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    size_t remaining = futures.size();

    while (remaining > 0 && std::chrono::steady_clock::now() < deadline) {
        for (size_t i = 0; i < futures.size(); ++i) {
            if (consumed[i]) {
                continue;
            }
            if (futures[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                auto result = futures[i].get();
                if (result) {
                    results.push_back(*result);
                }
                consumed[i] = true;
                --remaining;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (remaining > 0) {
        g_running = false;
        log(std::to_string(remaining) + " port probe(s) timed out", "WARNING");
    }

    std::cout << "\n============================================================\n";
    std::cout << "Detailed scan results\n";
    std::cout << "============================================================\n";
    if (results.empty()) {
        std::cout << "No usable AT ports found.\n";
    } else {
        for (size_t i = 0; i < results.size(); ++i) {
            std::cout << "[" << (i + 1) << "] " << results[i].device
                      << " @ " << results[i].baudrate << "\n";
            std::cout << "    " << results[i].info.substr(0, 160) << "\n";
        }
    }
    std::cout << "============================================================\n\n";
    return results;
}

std::string classify_interface(const std::string& name) {
    const std::string lower = to_lower(name);
    if (name == "lo") return "loopback";
    if (starts_with(lower, "enx")) return "usb";
    if (starts_with(lower, "wwan")) return "wwan";
    if (starts_with(lower, "usb")) return "usb";
    if (starts_with(lower, "eth") || starts_with(lower, "enp") || starts_with(lower, "eno")) return "ethernet";
    if (starts_with(lower, "wlan") || starts_with(lower, "wl")) return "wifi";
    if (lower.find("docker") != std::string::npos || starts_with(lower, "br-")) return "virtual";
    return "unknown";
}

std::vector<NetworkInterface> list_network_interfaces() {
    std::map<std::string, NetworkInterface> interfaces;
#ifdef _WIN32
    ULONG buffer_size = 15000;
    std::vector<unsigned char> buffer(buffer_size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG adapter_result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &buffer_size);
    if (adapter_result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        adapter_result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &buffer_size);
    }
    if (adapter_result != NO_ERROR) {
        return {};
    }

    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        NetworkInterface item;
        item.name = wide_to_utf8(adapter->FriendlyName);
        item.type = adapter->IfType == IF_TYPE_ETHERNET_CSMACD ? "ethernet" : "unknown";
        if (adapter->PhysicalAddressLength > 0) {
            std::ostringstream mac;
            for (ULONG i = 0; i < adapter->PhysicalAddressLength; ++i) {
                if (i) mac << '-';
                mac << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(adapter->PhysicalAddress[i]);
            }
            item.mac = mac.str();
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN]{};
                auto* addr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                item.ipv4 = ip;
                break;
            }
        }
        interfaces[item.name] = item;
    }
#else
    ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) {
        return {};
    }
    for (ifaddrs* it = addrs; it; it = it->ifa_next) {
        if (!it->ifa_name) {
            continue;
        }
        auto& item = interfaces[it->ifa_name];
        item.name = it->ifa_name;
        item.type = classify_interface(item.name);
        if (!it->ifa_addr) {
            continue;
        }
        if (it->ifa_addr->sa_family == AF_PACKET) {
            auto* packet = reinterpret_cast<sockaddr_ll*>(it->ifa_addr);
            if (packet->sll_halen == 6) {
                std::ostringstream mac;
                for (int i = 0; i < 6; ++i) {
                    if (i) mac << ':';
                    mac << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(packet->sll_addr[i]));
                }
                item.mac = mac.str();
            }
        } else if (it->ifa_addr->sa_family == AF_INET) {
            char ip[INET_ADDRSTRLEN]{};
            auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
            item.ipv4 = ip;
        }
    }
    freeifaddrs(addrs);
#endif

    std::vector<NetworkInterface> result;
    for (auto& [_, item] : interfaces) {
        if (item.mac.empty()) item.mac = "N/A";
        if (item.ipv4.empty()) item.ipv4 = "N/A";
        if (item.type.empty()) item.type = classify_interface(item.name);
        result.push_back(item);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    return result;
}

void print_network_interfaces(const std::vector<NetworkInterface>& interfaces) {
    std::cout << "\n======================================================================\n";
    std::cout << std::left << std::setw(32) << "Name"
              << std::setw(12) << "Type"
              << std::setw(22) << "MAC"
              << "IPv4\n";
    std::cout << "======================================================================\n";
    for (const auto& item : interfaces) {
        std::cout << std::left << std::setw(32) << item.name.substr(0, 31)
                  << std::setw(12) << item.type
                  << std::setw(22) << item.mac
                  << item.ipv4 << '\n';
    }
    std::cout << "======================================================================\n\n";
}

std::optional<std::string> find_interface(const Options& options) {
    const auto interfaces = list_network_interfaces();
    if (!options.mac_address.empty()) {
        const std::string target = normalize_mac(options.mac_address);
        for (const auto& item : interfaces) {
            if (normalize_mac(item.mac) == target || normalize_mac(item.name).find(target) != std::string::npos) {
                log("matched interface by MAC: " + item.name);
                return item.name;
            }
        }
    }

    std::vector<NetworkInterface> candidates;
    for (const auto& item : interfaces) {
        if (item.name == "lo" || item.type == "loopback" || item.type == "virtual") {
            continue;
        }
        if (item.type == "usb" || item.type == "wwan" || starts_with(to_lower(item.name), "enx")) {
            candidates.push_back(item);
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        auto score = [](const NetworkInterface& item) {
            if (starts_with(to_lower(item.name), "enx")) return 3;
            if (item.type == "wwan") return 2;
            if (item.type == "usb") return 1;
            return 0;
        };
        return score(a) > score(b);
    });

    if (options.auto_select || candidates.size() == 1) {
        log("auto-selected interface: " + candidates.front().name);
        return candidates.front().name;
    }

    print_network_interfaces(candidates);
    std::cout << "Select interface number, or press Enter for " << candidates.front().name << ": ";
    std::string line;
    std::getline(std::cin, line);
    line = trim(line);
    if (line.empty()) {
        return candidates.front().name;
    }
    const int index = std::atoi(line.c_str());
    if (index >= 1 && static_cast<size_t>(index) <= candidates.size()) {
        return candidates[static_cast<size_t>(index - 1)].name;
    }
    return candidates.front().name;
}

bool reset_network_interface(const std::string& interface_name, bool dry_run) {
    if (interface_name.empty()) {
        return true;
    }
    log("reset network interface: " + interface_name);
#ifdef _WIN32
    const std::string name = quote_arg(interface_name);
    run_command("netsh interface set interface name=" + name + " admin=disabled", dry_run);
    run_command("netsh interface set interface name=" + name + " admin=enabled", dry_run);
#else
    const std::string dev = quote_arg(interface_name);
    run_command("sudo ip link set " + dev + " down", dry_run);
    run_command("sudo ip addr flush dev " + dev, dry_run);
    run_command("sudo ip route flush dev " + dev, dry_run);
    run_command("sudo ip link set " + dev + " up", dry_run);
#endif
    return true;
}

bool configure_network(const std::string& interface_name,
                       const std::string& ip,
                       const std::vector<std::string>& dns,
                       bool dry_run) {
    if (interface_name.empty() || ip.empty()) {
        return false;
    }
    std::vector<std::string> parts;
    std::stringstream stream(ip);
    std::string segment;
    while (std::getline(stream, segment, '.')) {
        parts.push_back(segment);
    }
    if (parts.size() != 4) {
        log("invalid IPv4 address: " + ip, "ERROR");
        return false;
    }
    const std::string gateway = parts[0] + "." + parts[1] + "." + parts[2] + ".1";

#ifdef _WIN32
    const std::string name = quote_arg(interface_name);
    run_command("netsh interface ip set address name=" + name + " static " + ip + " 255.255.255.0 " + gateway, dry_run);
    if (!dns.empty()) {
        run_command("netsh interface ip set dns name=" + name + " static " + dns[0], dry_run);
    }
    if (dns.size() > 1) {
        run_command("netsh interface ip add dns name=" + name + " " + dns[1] + " index=2", dry_run);
    }
#else
    const std::string dev = quote_arg(interface_name);
    run_command("sudo ip addr add " + ip + "/24 dev " + dev, dry_run);
    run_command("sudo ip route replace default via " + gateway + " dev " + dev, dry_run);
    if (!dns.empty()) {
        std::ofstream resolv("/tmp/fm350gl-resolv.conf");
        for (const auto& server : dns) {
            resolv << "nameserver " << server << '\n';
        }
        resolv.close();
        run_command("sudo mv /tmp/fm350gl-resolv.conf /etc/resolv.conf", dry_run);
    }
#endif
    log("network configured: " + interface_name + " ip=" + ip);
    return true;
}

bool connection_is_active(SerialPort& serial) {
    const std::string response = send_at(serial, "AT+CGPADDR=3", 500, 512);
    return response.find("ERROR") == std::string::npos && first_ipv4(response).has_value();
}

bool dial_once(const Options& options, const std::string& interface_name) {
    try {
        SerialPort serial(options.port, options.baudrate, 3000);

        if (send_at(serial, "AT", 500, 256).find("OK") == std::string::npos) {
            log("module is not ready", "ERROR");
            return false;
        }

        const std::string sim = send_at(serial, "AT+CPIN?", 500, 256);
        if (sim.find("READY") == std::string::npos) {
            log("SIM is not ready: " + trim(sim), "ERROR");
            return false;
        }

        log("signal: " + trim(send_at(serial, "AT+CSQ", 500, 256)));

        const std::string apn_cmd = "AT+CGDCONT=3,\"ipv4v6\",\"" + options.apn + "\"";
        const std::string apn_response = send_at(serial, apn_cmd, 1000, 512);
        if (apn_response.find("OK") == std::string::npos) {
            log("failed to set APN: " + trim(apn_response), "ERROR");
            return false;
        }

        const std::string active = send_at(serial, "AT+CGACT=1,3", 3000, 512);
        if (active.find("ERROR") != std::string::npos) {
            log("PDP activation failed: " + trim(active), "ERROR");
            return false;
        }

        const std::string ip_response = send_at(serial, "AT+CGPADDR=3", 1000, 512);
        const auto ip = first_ipv4(ip_response);
        if (!ip) {
            log("no IP address returned: " + trim(ip_response), "ERROR");
            return false;
        }
        log("module IP: " + *ip);

        const std::string dns_response = send_at(serial, "AT+CGCONTRDP=3", 1000, 1024);
        auto dns = all_ipv4(dns_response);
        if (dns.size() > 2) {
            dns = {dns[1], dns[2]};
        } else if (dns.size() >= 2) {
            dns = {dns[0], dns[1]};
        } else {
            dns = {"8.8.8.8", "8.8.4.4"};
        }
        log("DNS: " + dns[0] + (dns.size() > 1 ? ", " + dns[1] : ""));

        if (!interface_name.empty()) {
            configure_network(interface_name, *ip, dns, options.dry_run);
        } else {
            log("no network interface selected; skipped local network configuration", "WARNING");
        }

        log("monitoring connection; press Ctrl+C to stop");
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!connection_is_active(serial)) {
                log("connection dropped", "WARNING");
                return false;
            }
            log("connection active", "DEBUG");
        }
        return true;
    } catch (const std::exception& error) {
        log(error.what(), "ERROR");
        return false;
    }
}

void auto_dial_loop(Options options) {
    if (options.port.empty()) {
        log("--auto-dial requires --port", "ERROR");
        return;
    }

    std::string interface_name = options.interface_name;
    if (interface_name.empty()) {
        if (auto found = find_interface(options)) {
            interface_name = *found;
        }
    }

    while (g_running) {
        reset_network_interface(interface_name, options.dry_run);
        if (dial_once(options, interface_name)) {
            break;
        }
        if (!g_running) {
            break;
        }
        log("dial failed or disconnected; retrying in 10 seconds", "WARNING");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

void print_help(const char* program) {
    std::cout << "FM350GL AT dialer\n\n"
              << "Usage:\n"
              << "  " << program << " --scan\n"
              << "  " << program << " --detailed-scan\n"
              << "  " << program << " --list-interfaces\n"
              << "  " << program << " --auto-dial --port COM5 --interface \"Ethernet 2\"\n"
              << "  " << program << " --auto-dial --port /dev/ttyUSB2 --interface wwan0\n\n"
              << "Options:\n"
              << "  -s, --scan                  List serial ports\n"
              << "  -d, --detailed-scan         Probe ports with AT/ATI\n"
              << "  -l, --list-interfaces       List network interfaces\n"
              << "  -a, --auto-dial             Dial, configure network, and monitor\n"
              << "  -p, --port <port>           Serial port, e.g. COM5 or /dev/ttyUSB2\n"
              << "  -b, --baudrate <baud>       Baudrate, default 115200\n"
              << "      --apn <apn>             APN, default ctnet\n"
              << "  -i, --interface <name>      Network interface to configure\n"
              << "      --mac <mac>             Select interface by MAC address\n"
              << "      --auto-select           Pick the best USB/WWAN interface automatically\n"
              << "      --dry-run               Print network commands without running them\n"
              << "  -h, --help                  Show this help\n";
}

Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(name + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "-s" || arg == "--scan" || arg == "--scan-only") {
            options.scan_only = true;
        } else if (arg == "-d" || arg == "--detailed-scan") {
            options.detailed_scan = true;
        } else if (arg == "-l" || arg == "--list-interfaces") {
            options.list_interfaces = true;
        } else if (arg == "-a" || arg == "--auto-dial") {
            options.auto_dial = true;
        } else if (arg == "-p" || arg == "--port") {
            options.port = need_value(arg);
        } else if (arg == "-b" || arg == "--baudrate") {
            options.baudrate = std::stoi(need_value(arg));
        } else if (arg == "--apn") {
            options.apn = need_value(arg);
        } else if (arg == "-i" || arg == "--interface") {
            options.interface_name = need_value(arg);
        } else if (arg == "--mac" || arg == "--mac-address") {
            options.mac_address = need_value(arg);
        } else if (arg == "--auto-select") {
            options.auto_select = true;
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }
    return options;
}

void handle_signal(int) {
    g_running = false;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        const Options options = parse_args(argc, argv);

        if (options.list_interfaces) {
            print_network_interfaces(list_network_interfaces());
            return 0;
        }

        if (options.detailed_scan) {
            const auto ports = scan_serial_ports();
            display_ports(ports);
            detailed_scan(ports);
            return 0;
        }

        if (options.auto_dial) {
            auto_dial_loop(options);
            return 0;
        }

        display_ports(scan_serial_ports());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
