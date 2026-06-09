# fm350gl-at-dialer

A small C++17 command-line dialer for FM350GL-style cellular modules. It sends
AT commands over a serial port, activates PDP context 3, reads IP/DNS settings,
and applies those settings to a selected local network interface.

The original Python scripts in `docs/python-reference/` are kept as behavior
references. The C++ version combines the useful Linux and Windows paths into one
cross-platform executable.

## What it does

- Lists serial ports.
- Probes serial ports with `AT` and `ATI`.
- Lists local network interfaces and MAC addresses.
- Sets APN with `AT+CGDCONT=3,"ipv4v6","<apn>"`.
- Activates PDP with `AT+CGACT=1,3`.
- Reads module IP with `AT+CGPADDR=3`.
- Reads DNS with `AT+CGCONTRDP=3`.
- Configures the host interface with `netsh` on Windows or `ip` on Linux.

## Build

```bash
cmake -S . -B build
cmake --build build
```

On Windows, the executable is usually created at
`build/fm350gl-at-dialer.exe` or under a configuration folder depending on the
generator. On Linux, it is usually `build/fm350gl-at-dialer`.

## Examples

```bash
# List serial ports
fm350gl-at-dialer --scan

# Probe AT ports
fm350gl-at-dialer --detailed-scan

# List network interfaces
fm350gl-at-dialer --list-interfaces

# Windows example
fm350gl-at-dialer --auto-dial --port COM5 --interface "Ethernet 2" --apn ctnet

# Linux example
sudo fm350gl-at-dialer --auto-dial --port /dev/ttyUSB2 --interface wwan0 --apn ctnet

# Select interface by MAC address
fm350gl-at-dialer --auto-dial --port COM5 --mac 00-00-11-12-13-14

# Preview network commands without applying them
fm350gl-at-dialer --auto-dial --port COM5 --interface "Ethernet 2" --dry-run
```

## Notes

- Applying network settings requires Administrator on Windows or root/sudo on
  Linux.
- Serial port access may also require suitable permissions.
- The gateway is inferred by replacing the last IPv4 octet with `1`, matching
  the original scripts.
- The tool name intentionally says `AT dialer` instead of `PPPoE`; the original
  scripts use AT commands and host interface configuration, not a PPPoE session.

## License

This project is released under the Unlicense. You may use, copy, modify,
publish, distribute, and sell it for commercial or non-commercial purposes.

## Repository layout

```text
.
├── CMakeLists.txt
├── src/
│   └── main.cpp
└── docs/
    └── python-reference/
        ├── Fm350gl-linux-pppoe.py
        └── Fm350gl-windows-pppoe.py
```
