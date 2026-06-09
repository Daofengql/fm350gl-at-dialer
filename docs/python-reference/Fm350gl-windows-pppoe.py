import serial.tools.list_ports
import serial
import subprocess
import time
import psutil
import socket
import os
import re

def scan_network_interfaces():
    return psutil.net_if_addrs()

def scan_serial_ports():
    while True:
        ports = serial.tools.list_ports.comports()
        at_ports = [port.device for port in ports if "AT" in port.description]
        if at_ports:
            return at_ports
        print("未找到AT端口，等待重试...")
        time.sleep(5)  # 每隔5秒重试一次


def check_module_ready(ser):
    while True:
        ser.write(b'AT\r\n')  # 发送AT指令
        response = ser.read(100)  # 读取响应
        if b'OK' in response:
            return True
        else:
            print("模组未准备好，等待重试...")
            time.sleep(2)

def check_sim_ready(ser):
    while True:
        ser.write(b'AT+CPIN?\r\n')  # 发送AT指令
        response = ser.read(100)  # 读取响应
        if b'+CPIN: READY' in response:
            return True
        else:
            print("SIM卡未准备好，等待重试...")
            time.sleep(2)
            

def execute_at_commands(port):
    ser = serial.Serial(port, 115200, timeout=1)  # 设置串口参数
    print("正在检查模组是否准备好...")
    if not check_module_ready(ser):
        print("模组未准备好，退出程序。")
        ser.close()
        return
    
    ser.write(b'ATI\r\n')  # 发送AT指令
    response = ser.read(100)  # 读取响应
    print(repr(response.decode()))


    print("模组已准备好，正在检查SIM卡状态...")
    if not check_sim_ready(ser):
        print("SIM卡未准备好，退出程序。")
        ser.close()
        return

    print("SIM卡已准备好，设置APN...")
    ser.write(b'AT+CGDCONT=3,"ipv4v6","cbnet"\r\n')  # 设置APN
    response = ser.read(100)  # 读取设置APN的响应
    print("APN设置 {}: {}".format(port, response.decode()))

    if "OK" in response.decode():
        ser.write(b'at+cgact=1,3\r\n')  
        response = ser.read(100)  # 读取激活PDP的响应
        print("激活PDP {}: {}".format(port, response.decode()))

        if "ACT" in response.decode():
            ser.write(b'at+cgpaddr=3\r\n')  
            response = ser.read(100)  # 读取IP地址的响应
            print("IP地址 {}: {}".format(port, response.decode()))
            ip_address = extract_ip_address(response.decode())

            ser.write(b'AT+CGCONTRDP=3\r\n')  # 发送AT指令获取DNS地址
            response = ser.read_until(b'OK').decode()  # 读取响应直到出现OK
            print("DNS设置:", response)

            # 使用正则表达式提取第一行的DNS地址
            dns_addresses_match = re.search(r'(\d+\.\d+\.\d+\.\d+)",\s*"(\d+\.\d+\.\d+\.\d+)"', response)
            if dns_addresses_match:
                dns_addresses = [dns_addresses_match.group(1), dns_addresses_match.group(2)]
                print("解析的DNS地址:", dns_addresses)
            else:
                print("无法解析DNS地址。")

            ser.close()
            return ip_address, dns_addresses

    ser.close()
    return None, None


def extract_ip_address(response):
    # 从响应中提取IP地址
    match = re.search(r'(\d+\.\d+\.\d+\.\d+)', response)
    if match:
        return match.group(1)
    else:
        return None



def execute_dns_commands(ser):
    ser.write(b'AT+CGCONTRDP=3\r\n')  # 发送AT指令
    response = ser.read(200)  # 读取响应
    print("DNS设置:", response.decode())
    return extract_dns_addresses(response.decode())

def modify_interface_ip(interface_name, ip_address, dns_addresses):
    # 构造合法的网关地址，将IP地址的最后一位改为1
    ip_parts = ip_address.split('.')
    gateway = '.'.join(ip_parts[:-1]) + '.1'

    # 设置IP地址
    result = subprocess.run(['netsh', 'interface', 'ip', 'set', 'address', 'name=' + interface_name, 'static', ip_address, '255.255.255.0', gateway], capture_output=True, text=True)
    
    # 设置DNS地址
    subprocess.run(['netsh', 'interface', 'ip', 'set', 'dns', 'name=' + interface_name, 'static', dns_addresses[0]])
    if len(dns_addresses) > 1:
        subprocess.run(['netsh', 'interface', 'ip', 'add', 'dns', 'name=' + interface_name, dns_addresses[1], 'index=2'])

    # 打印设置结果
#     print("设置IP地址结果：", result)
#     if result.returncode == 0:
#         print(f"设置IP地址成功：{ip_address}")
#     else:
#         print("设置IP地址失败：", result.stderr)

        
def get_mac_address(interface_name):
    addrs = psutil.net_if_addrs()
    if interface_name in addrs:
        for addr in addrs[interface_name]:
            if addr.family == psutil.AF_LINK:
                return addr.address
    return None



def check_ping(hostname):
    # Ping指定主机10次
    ping_cmd = ["ping", "-n", "10", hostname]
    ping_process = subprocess.Popen(ping_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ping_output, _ = ping_process.communicate()
    return ping_output.decode()



if __name__ == "__main__":
    # 扫描串口并执行AT指令
    at_ports = scan_serial_ports()
    if at_ports:
        print("Found AT ports:", at_ports)
        for port in at_ports:
            ip_address, dns_addresses = execute_at_commands(port)
            if ip_address:
                # 扫描所有网卡
                interfaces = scan_network_interfaces()
                # 查找指定MAC地址的网卡
                target_interface = None  # 初始化目标网卡变量
                for interface_name, addresses in interfaces.items():
                    for address in addresses:
                        # print(f"Interface: {interface_name}, MAC Address: {address.address}")
                        if address.family == psutil.AF_LINK and get_mac_address(interface_name) == "00-00-11-12-13-14":
                            target_interface = interface_name
                            break

                    if target_interface:
                        break  # 如果找到目标网卡，就退出循环

                # 修改指定网卡的IP地址和DNS地址
                if target_interface:
                    modify_interface_ip(target_interface, ip_address, dns_addresses)
                    print(f"IP地址成功设置为 {ip_address}，DNS地址成功设置为 {dns_addresses}，网卡名称为 {target_interface}")

                    # 再次读取网卡信息，检查是否成功修改IP地址
                    interfaces_after = scan_network_interfaces()
                    if target_interface in interfaces_after:
                        print("设置成功后的网卡信息：")
                        for addr in interfaces_after[target_interface]:
                            if hasattr(addr, 'family') and addr.family == socket.AF_INET:
                                print(f"IPv4地址：{addr.address}")
                                print(f"子网掩码：{addr.netmask}")
                                print(f"网关地址：{addr.broadcast}")
                                print(f"DNS地址：{dns_addresses}")
                            elif hasattr(addr, 'family') and addr.family == psutil.AF_LINK:
                                print(f"MAC地址：{addr.address}")
                    else:
                        print("未找到修改后的网卡信息。")

                else:
                    print("设置IP地址失败：", result.stderr)
