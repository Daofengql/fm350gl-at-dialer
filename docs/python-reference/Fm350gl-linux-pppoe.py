#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FM350GL Ubuntu拨号管理器 - 改进版
支持多线程端口扫描、自动拨号、连接监控
"""

import serial.tools.list_ports
import serial
import subprocess
import time
import os
import re
import sys
import argparse
import threading
import queue
import signal
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import netifaces

class FM350GLDialer:
    def __init__(self, args):
        self.args = args
        self.apn = args.apn
        self.port = args.port
        self.baudrate = args.baudrate
        self.interface = args.interface
        self.target_mac = args.mac_address
        self.scan_results = queue.Queue()
        self.working_ports = []
        self.running = True
        self.connection_active = False
        
    def log(self, message, level="INFO"):
        """带时间戳的日志输出"""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] [{level}] {message}")
    
    def scan_usb_serial_ports(self):
        """扫描所有USB串口设备"""
        self.log("扫描USB串口设备...")
        ports = serial.tools.list_ports.comports()
        usb_ports = []
        
        for port in ports:
            # 只处理ttyUSB设备
            if port.device.startswith('/dev/ttyUSB'):
                usb_ports.append(port)
                
        return usb_ports
    
    def display_port_info(self, ports):
        """显示端口详细信息"""
        if not ports:
            self.log("未发现任何ttyUSB设备", "WARNING")
            return
            
        print("\n" + "="*60)
        print("USB串口设备列表")
        print("="*60)
        
        for i, port in enumerate(ports, 1):
            print(f"\n[{i}] 设备: {port.device}")
            print(f"    描述: {port.description}")
            print(f"    硬件ID: {port.hwid}")
            if port.manufacturer:
                print(f"    制造商: {port.manufacturer}")
            if port.product:
                print(f"    产品: {port.product}")
            if port.vid and port.pid:
                print(f"    VID:PID: {port.vid:04X}:{port.pid:04X}")
            if port.serial_number:
                print(f"    序列号: {port.serial_number}")
        
        print("="*60 + "\n")
    
    def test_port_thread(self, port_info):
        """线程函数：测试单个端口"""
        port_device = port_info['device']
        baudrates = [115200, 9600, 19200, 38400, 57600, 230400, 460800, 921600]
        
        self.log(f"线程 {threading.current_thread().name}: 测试端口 {port_device}")
        
        for baudrate in baudrates:
            if not self.running:
                break
                
            try:
                ser = serial.Serial(port_device, baudrate, timeout=1)
                ser.reset_input_buffer()
                ser.reset_output_buffer()
                
                # 发送AT命令
                ser.write(b'AT\r\n')
                time.sleep(0.1)
                response = ser.read(100)
                
                if response and b'OK' in response:
                    # 获取模块信息
                    ser.write(b'ATI\r\n')
                    time.sleep(0.2)
                    info_response = ser.read(500)
                    info_str = info_response.decode('utf-8', errors='ignore').strip()
                    
                    result = {
                        'device': port_device,
                        'baudrate': baudrate,
                        'info': info_str,
                        'port_info': port_info
                    }
                    
                    self.scan_results.put(result)
                    self.log(f"✓ 端口 {port_device} 响应正常 (波特率: {baudrate})")
                    
                    ser.close()
                    return result
                    
                ser.close()
                
            except Exception as e:
                continue
        
        self.log(f"✗ 端口 {port_device} 无响应", "DEBUG")
        return None
    
    def parallel_port_scan(self, ports):
        """并行扫描多个端口"""
        self.log(f"开始并行扫描 {len(ports)} 个端口...")
        
        # 准备端口信息
        port_infos = []
        for port in ports:
            port_infos.append({
                'device': port.device,
                'description': port.description,
                'hwid': port.hwid,
                'manufacturer': port.manufacturer,
                'product': port.product,
                'vid': port.vid,
                'pid': port.pid
            })
        
        # 使用线程池并行测试
        with ThreadPoolExecutor(max_workers=min(len(ports), 8)) as executor:
            future_to_port = {
                executor.submit(self.test_port_thread, port_info): port_info 
                for port_info in port_infos
            }
            
            for future in as_completed(future_to_port):
                try:
                    result = future.result()
                    if result:
                        self.working_ports.append(result)
                except Exception as e:
                    self.log(f"端口测试异常: {e}", "ERROR")
        
        # 显示扫描结果
        print("\n" + "="*60)
        print("扫描结果")
        print("="*60)
        
        if self.working_ports:
            for i, port in enumerate(self.working_ports, 1):
                print(f"\n[{i}] 可用端口: {port['device']}")
                print(f"    波特率: {port['baudrate']}")
                print(f"    模块信息: {port['info'][:100]}...")
        else:
            print("\n未发现可用的AT端口")
        
        print("="*60 + "\n")
        
        return self.working_ports
    
    def find_interface_by_mac(self, target_mac):
        """根据MAC地址查找网络接口"""
        try:
            # 处理MAC地址格式
            mac_clean = target_mac.replace(':', '').replace('-', '').lower()
            
            interfaces = netifaces.interfaces()
            for interface in interfaces:
                # 检查enx格式的接口名（enx + MAC地址）
                if interface.startswith('enx') and mac_clean in interface.lower():
                    self.log(f"通过接口名找到匹配: {interface}")
                    return interface
                
                # 常规MAC地址匹配
                try:
                    addrs = netifaces.ifaddresses(interface)
                    if netifaces.AF_LINK in addrs:
                        for addr in addrs[netifaces.AF_LINK]:
                            if 'addr' in addr:
                                interface_mac = addr['addr'].replace(':', '').replace('-', '').lower()
                                if interface_mac == mac_clean:
                                    return interface
                except:
                    continue
        except Exception as e:
            self.log(f"查找MAC地址接口失败: {e}", "ERROR")
        return None
    
    def find_usb_network_interface(self):
        """自动查找USB网卡接口"""
        self.log("自动查找USB网卡...")
        
        # 首先尝试通过MAC地址查找
        if self.target_mac:
            interface = self.find_interface_by_mac(self.target_mac)
            if interface:
                self.log(f"通过MAC地址找到接口: {interface}")
                return interface
        
        # 查找所有网络接口
        interfaces = netifaces.interfaces()
        usb_interfaces = []
        
        # USB网卡接口名称模式（优先级排序）
        priority_patterns = [
            ('enx', 10),      # enx开头的接口（最高优先级）
            ('wwan', 8),      # WWAN接口
            ('usb', 7),       # USB接口
            ('wdm', 6),       # WDM接口
            ('cdc', 5),       # CDC接口
            ('eth', 3)        # eth接口（但不是eth0）
        ]
        
        for interface in interfaces:
            # 跳过本地回环和主网卡
            if interface in ['lo', 'eth0', 'eno1', 'enp0s3']:
                continue
            
            # 计算优先级分数
            priority_score = 0
            for pattern, score in priority_patterns:
                if interface.lower().startswith(pattern):
                    priority_score = score
                    break
            
            # 特别处理enx接口（通常是USB网卡）
            if interface.startswith('enx'):
                # 提取MAC地址
                mac_from_name = interface[3:]  # 去掉'enx'前缀
                if len(mac_from_name) == 12:  # 确保是MAC地址长度
                    # 格式化MAC地址
                    formatted_mac = ':'.join([mac_from_name[i:i+2] for i in range(0, 12, 2)])
                    usb_interfaces.append({
                        'interface': interface,
                        'mac': formatted_mac,
                        'priority': priority_score,
                        'type': 'USB网卡'
                    })
                    continue
            
            # 检查是否有MAC地址
            if priority_score > 0:
                try:
                    addrs = netifaces.ifaddresses(interface)
                    if netifaces.AF_LINK in addrs:
                        mac_addr = addrs[netifaces.AF_LINK][0].get('addr', '')
                        if mac_addr and mac_addr != '00:00:00:00:00:00':
                            interface_type = 'USB网卡' if priority_score >= 7 else '其他网卡'
                            usb_interfaces.append({
                                'interface': interface,
                                'mac': mac_addr,
                                'priority': priority_score,
                                'type': interface_type
                            })
                except:
                    continue
        
        # 按优先级排序
        usb_interfaces.sort(key=lambda x: x['priority'], reverse=True)
        
        # 显示找到的网卡
        if usb_interfaces:
            self.log(f"找到 {len(usb_interfaces)} 个可能的网卡:")
            for i, info in enumerate(usb_interfaces, 1):
                print(f"  [{i}] {info['interface']} ({info['type']})")
                print(f"      MAC: {info['mac']}")
            
            # 如果只有一个enx接口，自动选择
            enx_interfaces = [i for i in usb_interfaces if i['interface'].startswith('enx')]
            if len(enx_interfaces) == 1:
                selected = enx_interfaces[0]['interface']
                self.log(f"自动选择USB网卡: {selected}")
                return selected
            
            # 如果只有一个接口，自动选择
            if len(usb_interfaces) == 1:
                selected = usb_interfaces[0]['interface']
                self.log(f"自动选择唯一的网卡: {selected}")
                return selected
            
            # 多个接口时的处理
            if not self.args.auto_select:
                # 优先推荐enx接口
                if enx_interfaces:
                    print(f"\n推荐使用: {enx_interfaces[0]['interface']} (USB网卡)")
                
                print("\n请选择要使用的网卡 (输入编号或直接回车使用推荐):")
                try:
                    choice = input().strip()
                    if not choice and enx_interfaces:
                        # 直接回车使用推荐的enx接口
                        selected = enx_interfaces[0]['interface']
                        self.log(f"使用推荐的接口: {selected}")
                        return selected
                    elif choice.isdigit() and 1 <= int(choice) <= len(usb_interfaces):
                        selected = usb_interfaces[int(choice)-1]['interface']
                        self.log(f"选择了接口: {selected}")
                        return selected
                except:
                    pass
            else:
                # 自动选择优先级最高的
                selected = usb_interfaces[0]['interface']
                self.log(f"自动选择优先级最高的网卡: {selected}")
                return selected
        
        self.log("未找到USB网卡", "WARNING")
        return None
    
    def list_all_interfaces(self):
        """列出所有网络接口"""
        self.log("所有网络接口:")
        interfaces = netifaces.interfaces()
        
        # 对接口进行分类
        interface_list = []
        
        for interface in interfaces:
            try:
                addrs = netifaces.ifaddresses(interface)
                mac = 'N/A'
                ipv4 = 'N/A'
                interface_type = '未知'
                
                # MAC地址
                if netifaces.AF_LINK in addrs:
                    mac = addrs[netifaces.AF_LINK][0].get('addr', 'N/A')
                
                # IPv4地址
                if netifaces.AF_INET in addrs:
                    ipv4 = addrs[netifaces.AF_INET][0].get('addr', 'N/A')
                
                # 判断接口类型
                if interface == 'lo':
                    interface_type = '本地回环'
                elif interface.startswith('enx'):
                    interface_type = 'USB网卡'
                elif interface.startswith('eth'):
                    interface_type = '以太网'
                elif interface.startswith('wlan'):
                    interface_type = '无线网卡'
                elif interface.startswith('wwan'):
                    interface_type = 'WWAN接口'
                elif 'docker' in interface or 'br-' in interface:
                    interface_type = 'Docker接口'
                elif 'veth' in interface:
                    interface_type = '虚拟接口'
                
                interface_list.append({
                    'name': interface,
                    'type': interface_type,
                    'mac': mac,
                    'ipv4': ipv4
                })
            except:
                interface_list.append({
                    'name': interface,
                    'type': '读取失败',
                    'mac': 'N/A',
                    'ipv4': 'N/A'
                })
        
        # 按类型分组显示
        print("\n" + "="*70)
        print(f"{'接口名称':<20} {'类型':<15} {'MAC地址':<20} {'IPv4地址':<15}")
        print("="*70)
        
        # 优先显示USB网卡
        for intf in interface_list:
            if intf['type'] == 'USB网卡':
                print(f"{intf['name']:<20} {intf['type']:<15} {intf['mac']:<20} {intf['ipv4']:<15}")
        
        # 然后显示其他物理接口
        for intf in interface_list:
            if intf['type'] in ['以太网', '无线网卡', 'WWAN接口']:
                print(f"{intf['name']:<20} {intf['type']:<15} {intf['mac']:<20} {intf['ipv4']:<15}")
        
        # 最后显示虚拟接口
        for intf in interface_list:
            if intf['type'] not in ['USB网卡', '以太网', '无线网卡', 'WWAN接口']:
                print(f"{intf['name']:<20} {intf['type']:<15} {intf['mac']:<20} {intf['ipv4']:<15}")
        
        print("="*70)
    
    def check_connection_status(self, ser):
        """检查连接状态"""
        try:
            ser.reset_input_buffer()
            ser.write(b'AT+CGPADDR=3\r\n')
            time.sleep(0.5)
            response = ser.read(200).decode('utf-8', errors='ignore')
            
            # 检查是否有有效IP地址
            if re.search(r'\d+\.\d+\.\d+\.\d+', response) and 'ERROR' not in response:
                return True
            return False
            
        except Exception as e:
            self.log(f"检查连接状态失败: {e}", "ERROR")
            return False
    
    def reset_network_interface(self, interface):
        """重置网络接口"""
        self.log(f"重置网络接口 {interface}...")
        try:
            commands = [
                ['sudo', 'ip', 'link', 'set', interface, 'down'],
                ['sudo', 'ip', 'addr', 'flush', 'dev', interface],
                ['sudo', 'ip', 'route', 'flush', 'dev', interface],
                ['sudo', 'ip', 'link', 'set', interface, 'up']
            ]
            
            for cmd in commands:
                subprocess.run(cmd, capture_output=True, text=True, timeout=5)
                
            self.log("网络接口重置完成")
            return True
            
        except Exception as e:
            self.log(f"重置网络接口失败: {e}", "ERROR")
            return False
    
    def dial_connection(self, port_device, baudrate):
        """执行拨号连接"""
        self.log(f"开始拨号 (端口: {port_device}, 波特率: {baudrate})")
        
        try:
            ser = serial.Serial(port_device, baudrate, timeout=3)
            
            # 检查模块就绪
            ser.write(b'AT\r\n')
            time.sleep(0.5)
            response = ser.read(100)
            if b'OK' not in response:
                self.log("模块未就绪", "ERROR")
                ser.close()
                return False
            
            # 检查SIM卡
            ser.write(b'AT+CPIN?\r\n')
            time.sleep(0.5)
            response = ser.read(100).decode('utf-8', errors='ignore')
            if 'READY' not in response:
                self.log(f"SIM卡未就绪: {response.strip()}", "ERROR")
                ser.close()
                return False
            
            # 检查信号
            ser.write(b'AT+CSQ\r\n')
            time.sleep(0.5)
            response = ser.read(100).decode('utf-8', errors='ignore')
            self.log(f"信号强度: {response.strip()}")
            
            # 设置APN
            self.log(f"设置APN: {self.apn}")
            apn_cmd = f'AT+CGDCONT=3,"ipv4v6","{self.apn}"\r\n'.encode()
            ser.write(apn_cmd)
            time.sleep(1)
            response = ser.read(100).decode('utf-8', errors='ignore')
            
            if 'OK' not in response:
                self.log(f"APN设置失败: {response.strip()}", "ERROR")
                ser.close()
                return False
            
            # 激活PDP
            self.log("激活PDP上下文...")
            ser.write(b'AT+CGACT=1,3\r\n')
            time.sleep(3)
            response = ser.read(200).decode('utf-8', errors='ignore')
            
            if 'ERROR' in response:
                self.log(f"PDP激活失败: {response.strip()}", "ERROR")
                ser.close()
                return False
            
            # 获取IP地址
            ser.write(b'AT+CGPADDR=3\r\n')
            time.sleep(1)
            response = ser.read(200).decode('utf-8', errors='ignore')
            ip_match = re.search(r'(\d+\.\d+\.\d+\.\d+)', response)
            
            if not ip_match:
                self.log("未获取到IP地址", "ERROR")
                ser.close()
                return False
            
            ip_address = ip_match.group(1)
            self.log(f"获取到IP地址: {ip_address}")
            
            # 获取DNS
            ser.write(b'AT+CGCONTRDP=3\r\n')
            time.sleep(1)
            response = ser.read(500).decode('utf-8', errors='ignore')
            dns_matches = re.findall(r'(\d+\.\d+\.\d+\.\d+)', response)
            dns_addresses = dns_matches[1:3] if len(dns_matches) > 2 else ['8.8.8.8', '8.8.4.4']
            self.log(f"DNS地址: {dns_addresses}")
            
            # 配置网络接口
            if self.interface:
                self.configure_network(self.interface, ip_address, dns_addresses)
            else:
                # 尝试自动查找USB网卡
                auto_interface = self.find_usb_network_interface()
                if auto_interface:
                    self.configure_network(auto_interface, ip_address, dns_addresses)
                else:
                    self.log("未找到可用的网络接口，跳过网络配置", "WARNING")
            
            self.connection_active = True
            
            # 保持连接并监控
            self.monitor_connection(ser)
            
        except Exception as e:
            self.log(f"拨号过程异常: {e}", "ERROR")
            return False
        
        finally:
            if 'ser' in locals():
                ser.close()
    
    def configure_network(self, interface, ip_address, dns_addresses):
        """配置网络接口"""
        try:
            # 计算网关
            ip_parts = ip_address.split('.')
            gateway = '.'.join(ip_parts[:-1]) + '.1'
            
            commands = [
                ['sudo', 'ip', 'addr', 'add', f"{ip_address}/24", 'dev', interface],
                ['sudo', 'ip', 'route', 'add', 'default', 'via', gateway, 'dev', interface]
            ]
            
            for cmd in commands:
                subprocess.run(cmd, capture_output=True, text=True)
            
            # 配置DNS
            dns_config = '\n'.join([f"nameserver {dns}" for dns in dns_addresses])
            with open('/tmp/resolv.conf.new', 'w') as f:
                f.write(dns_config + '\n')
            subprocess.run(['sudo', 'mv', '/tmp/resolv.conf.new', '/etc/resolv.conf'])
            
            self.log("网络配置完成")
            
        except Exception as e:
            self.log(f"网络配置失败: {e}", "ERROR")
    
    def monitor_connection(self, ser):
        """监控连接状态"""
        self.log("开始监控连接状态...")
        heartbeat_interval = 30  # 30秒心跳
        
        while self.running and self.connection_active:
            try:
                time.sleep(heartbeat_interval)
                
                if not self.check_connection_status(ser):
                    self.log("连接已断开，准备重新拨号", "WARNING")
                    self.connection_active = False
                    break
                else:
                    self.log("连接正常", "DEBUG")
                    
            except KeyboardInterrupt:
                self.log("用户中断")
                break
            except Exception as e:
                self.log(f"监控异常: {e}", "ERROR")
                self.connection_active = False
                break
    
    def auto_dial_loop(self):
        """自动拨号循环"""
        retry_delay = 10
        
        # 如果没有指定接口，尝试自动查找
        if not self.interface:
            self.interface = self.find_usb_network_interface()
        
        while self.running:
            try:
                # 重置网络接口
                if self.interface:
                    self.reset_network_interface(self.interface)
                
                # 执行拨号
                success = self.dial_connection(self.port, self.baudrate)
                
                if not success:
                    self.log(f"拨号失败，{retry_delay}秒后重试...", "WARNING")
                    time.sleep(retry_delay)
                    continue
                
                # 连接断开后等待重试
                self.log(f"连接断开，{retry_delay}秒后重新拨号...", "WARNING")
                time.sleep(retry_delay)
                
            except KeyboardInterrupt:
                self.log("用户中断自动拨号")
                break
            except Exception as e:
                self.log(f"自动拨号异常: {e}", "ERROR")
                time.sleep(retry_delay)
    
    def run(self):
        """主运行函数"""
        # 列出接口模式
        if self.args.list_interfaces:
            self.list_all_interfaces()
            return
            ports = self.scan_usb_serial_ports()
            self.display_port_info(ports)
            return
        
        # 详细扫描模式
        if self.args.detailed_scan:
            ports = self.scan_usb_serial_ports()
            if ports:
                self.parallel_port_scan(ports)
            else:
                self.log("未发现任何ttyUSB设备", "WARNING")
            return
        
        # 自动拨号模式
        if self.args.auto_dial:
            if not self.port:
                self.log("自动拨号模式需要指定端口 (-p/--port)", "ERROR")
                return
            
            self.log("启动自动拨号模式")
            self.auto_dial_loop()
            return
        
        # 默认：显示基本信息
        ports = self.scan_usb_serial_ports()
        self.display_port_info(ports)

def signal_handler(signum, frame):
    """信号处理函数"""
    print("\n收到中断信号，正在退出...")
    sys.exit(0)

def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description='FM350GL Ubuntu拨号管理器',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
  %(prog)s                     # 列出所有ttyUSB设备
  %(prog)s -s                  # 仅扫描端口信息
  %(prog)s -d                  # 详细扫描（多线程测试）
  %(prog)s -l                  # 列出所有网络接口
  %(prog)s -a -p /dev/ttyUSB2  # 自动拨号（自动查找USB网卡）
  %(prog)s -a -p /dev/ttyUSB2 --mac 00:00:11:12:13:14  # 通过MAC地址指定网卡
  %(prog)s -a -p /dev/ttyUSB2 -i wwan0  # 手动指定网卡
        '''
    )
    
    parser.add_argument('-s', '--scan-only', action='store_true',
                        help='仅扫描端口，显示设备信息')
    parser.add_argument('-d', '--detailed-scan', action='store_true',
                        help='详细扫描模式，测试所有端口和波特率')
    parser.add_argument('-l', '--list-interfaces', action='store_true',
                        help='列出所有网络接口')
    parser.add_argument('-a', '--auto-dial', action='store_true',
                        help='自动拨号模式，包含重连机制')
    parser.add_argument('-p', '--port', type=str,
                        help='指定串口设备 (如: /dev/ttyUSB2)')
    parser.add_argument('-b', '--baudrate', type=int, default=115200,
                        help='指定波特率 (默认: 115200)')
    parser.add_argument('--apn', type=str, default='ctnet',
                        help='指定APN (默认: ctnet)')
    parser.add_argument('-i', '--interface', type=str,
                        help='指定网络接口 (如: wwan0)')
    parser.add_argument('--mac', '--mac-address', dest='mac_address', type=str,
                        help='通过MAC地址指定网卡 (如: 00:00:11:12:13:14)')
    parser.add_argument('--auto-select', action='store_true',
                        help='自动选择第一个找到的USB网卡')
    
    args = parser.parse_args()
    
    # 注册信号处理
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # 检查权限
    if args.auto_dial and os.geteuid() != 0:
        print("错误: 自动拨号模式需要root权限，请使用sudo运行")
        sys.exit(1)
    
    # 创建并运行管理器
    dialer = FM350GLDialer(args)
    
    try:
        dialer.run()
    except Exception as e:
        print(f"程序异常: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()