# fm350gl-at-dialer

`fm350gl-at-dialer` 是一个 C++17 命令行工具，用于管理 FM350GL 这类蜂窝模块的 AT 指令拨号流程。它会通过串口发送 AT 指令，激活 PDP 上下文 3，读取模块返回的 IP/DNS 信息，并把这些网络参数配置到本机指定网卡上。

原始 Python 脚本保留在 `docs/python-reference/` 中，方便对照行为。当前 C++ 版本把 Linux 和 Windows 两份脚本里的主要逻辑合并成了一个跨平台可执行程序。

## 功能

- 列出串口设备。
- 使用 `AT` 和 `ATI` 探测可用 AT 端口。
- 列出本机网卡、MAC 地址和 IPv4 地址。
- 通过 `AT+CGDCONT=3,"ipv4v6","<apn>"` 设置 APN。
- 通过 `AT+CGACT=1,3` 激活 PDP 上下文。
- 通过 `AT+CGPADDR=3` 读取模块 IP。
- 通过 `AT+CGCONTRDP=3` 读取 DNS。
- Windows 使用 `netsh` 配置网卡，Linux 使用 `ip` 配置网卡。

## 构建

```bash
cmake -S . -B build
cmake --build build
```

Windows 下可执行文件通常位于 `build/fm350gl-at-dialer.exe`，或位于生成器对应的配置目录中。Linux 下通常位于 `build/fm350gl-at-dialer`。

## 使用示例

```bash
# 列出串口
fm350gl-at-dialer --scan

# 探测 AT 端口
fm350gl-at-dialer --detailed-scan

# 列出网卡
fm350gl-at-dialer --list-interfaces

# Windows 示例
fm350gl-at-dialer --auto-dial --port COM5 --interface "Ethernet 2" --apn ctnet

# Linux 示例
sudo fm350gl-at-dialer --auto-dial --port /dev/ttyUSB2 --interface wwan0 --apn ctnet

# 通过 MAC 地址选择网卡
fm350gl-at-dialer --auto-dial --port COM5 --mac 00-00-11-12-13-14

# 只打印网络配置命令，不真正执行
fm350gl-at-dialer --auto-dial --port COM5 --interface "Ethernet 2" --dry-run
```

## 注意事项

- 修改网络配置需要权限：Windows 需要管理员权限，Linux 需要 root 或 sudo。
- 访问串口也可能需要额外权限。
- 网关地址会按原脚本逻辑推断：把 IPv4 地址最后一段替换为 `1`。
- 项目名使用 `AT dialer`，没有继续使用 `PPPoE`，因为原脚本实际做的是 AT 指令拨号和本机网卡配置，并不是建立 PPPoE 会话。

## 自动发布

仓库包含 GitHub Actions 发布工作流：`.github/workflows/release.yml`。

推送 `v*` 标签时会自动：

- 在 Ubuntu 构建 Linux x64 和 Linux x86 可执行文件，并打包为 `.tar.gz`。
- 在 Windows 构建 Windows x64 和 Windows x86 可执行文件，并打包为 `.zip`。
- 创建 GitHub Release。
- 上传四个平台/架构的压缩包作为 Release Assets。

发布示例：

```bash
git tag v0.1.0
git push origin v0.1.0
```

如果同一个标签的 Release 已存在，工作流会覆盖上传同名构建产物。

## 许可证

本项目使用现成的 [The Unlicense](https://unlicense.org/) 许可证。你可以自由使用、复制、修改、发布、分发和商用。

## 仓库结构

```text
.
├── CMakeLists.txt
├── LICENSE
├── README.md
├── .github/
│   └── workflows/
│       └── release.yml
├── src/
│   └── main.cpp
└── docs/
    └── python-reference/
        ├── Fm350gl-linux-pppoe.py
        └── Fm350gl-windows-pppoe.py
```
