"""
DAPLink PC Bridge v1.0.0 - 跨网络无线调试桥接工具

连接到中继服务器（WebSocket 或 TCP），在本地创建 TCP 服务器供 elaphureLink 连接。

用法:
    python bridge.py                                        # 交互式输入
    python bridge.py --relay ws://server:7000 --code A3K7N2 # WebSocket 模式
    python bridge.py --relay server --code A3K7N2 --tcp     # TCP 模式 (端口 7001)

依赖:
    pip install websockets

打包为 exe:
    pip install pyinstaller
    pyinstaller --onefile bridge.py
"""

import asyncio
import argparse
import configparser
import json
import os
import socket
import struct
import sys
import signal

try:
    import websockets
except ImportError:
    print("请安装依赖: pip install websockets")
    sys.exit(1)

# Config file path (next to bridge.py)
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config.ini')


# elaphureLink protocol constants
EL_IDENTIFIER = 0x8A656C70
EL_CMD_HANDSHAKE = 0x00000000
EL_DAP_VERSION = 0x00000001

DEFAULT_TCP_PORT = 3240
DEFAULT_SERIAL_PORT = 3241
DEFAULT_RELAY_PORT = 7000
DEFAULT_RELAY_TCP_PORT = 7001

# WebSocket channel prefixes (must match ESP32 firmware)
WS_CHANNEL_DAP = 0x00
WS_CHANNEL_SERIAL = 0x01
WS_CHANNEL_RTT = 0x02
WS_CHANNEL_FLASH = 0x03

DEFAULT_RTT_PORT = 3242

# TCP relay registration roles
TCP_ROLE_DEVICE = 0x01
TCP_ROLE_CLIENT = 0x02
TCP_STATUS_REGISTERED = 0x00
TCP_STATUS_PAIRED = 0x01
TCP_STATUS_PEER_DISCONNECTED = 0x02


def check_port_available(port: int, host: str = '127.0.0.1') -> bool:
    """Check if a TCP port is available for binding."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind((host, port))
        return True
    except OSError:
        return False


class DAPLinkBridge:
    def __init__(self, relay_url: str, code: str, tcp_port: int = DEFAULT_TCP_PORT,
                 serial_port: int = DEFAULT_SERIAL_PORT, rtt_port: int = DEFAULT_RTT_PORT):
        self.relay_url = relay_url
        self.code = code.upper()
        self.tcp_port = tcp_port
        self.serial_port = serial_port
        self.rtt_port = rtt_port
        self.ws = None
        self.tcp_writer = None
        self.serial_writer = None
        self.rtt_writer = None
        self.paired = False
        self.running = True
        self.el_handshake_done = False
        self._pending_resp = asyncio.Queue()
        self._pending_serial = asyncio.Queue()
        self._pending_rtt = asyncio.Queue()

    async def connect_relay(self):
        """Connect to relay server and register as client."""
        print(f"正在连接中继服务器: {self.relay_url}")
        self.ws = await websockets.connect(self.relay_url, max_size=2**20, open_timeout=30)

        # Register with code
        reg = json.dumps({
            "type": "register",
            "role": "client",
            "code": self.code
        })
        await self.ws.send(reg)
        print(f"已注册，配对码: {self.code}，等待设备连接...")

        # Wait for pairing
        while self.running:
            try:
                msg = await asyncio.wait_for(self.ws.recv(), timeout=1.0)
                if isinstance(msg, str):
                    data = json.loads(msg)
                    if data.get("type") == "paired":
                        self.paired = True
                        print("✓ 设备已连接！")
                        return True
                    elif data.get("type") == "device_disconnected":
                        print("✗ 设备已断开")
                        self.paired = False
                        return False
            except asyncio.TimeoutError:
                continue
            except websockets.exceptions.ConnectionClosed:
                print("✗ 中继服务器连接断开")
                return False
        return False

    async def relay_reader(self):
        """Read responses from relay (from ESP32) and dispatch by channel."""
        try:
            async for msg in self.ws:
                if isinstance(msg, bytes) and self.paired:
                    if len(msg) > 1:
                        channel = msg[0]
                        payload = msg[1:]
                        if channel == WS_CHANNEL_DAP:
                            await self._pending_resp.put(payload)
                        elif channel == WS_CHANNEL_SERIAL:
                            await self._pending_serial.put(payload)
                        elif channel == WS_CHANNEL_RTT:
                            await self._pending_rtt.put(payload)
                    else:
                        # Legacy: no prefix, treat as DAP
                        await self._pending_resp.put(msg)
                elif isinstance(msg, str):
                    data = json.loads(msg)
                    if data.get("type") == "device_disconnected":
                        print("\n✗ 设备已断开连接")
                        self.paired = False
                        break
        except websockets.exceptions.ConnectionClosed:
            print("\n✗ 中继连接断开")
            self.paired = False

    async def handle_el_client(self, reader: asyncio.StreamReader,
                               writer: asyncio.StreamWriter):
        """Handle one elaphureLink TCP client connection."""
        addr = writer.get_extra_info('peername')
        print(f"elaphureLink 已连接: {addr}")
        self.tcp_writer = writer
        self.el_handshake_done = False

        # Clear any stale responses
        while not self._pending_resp.empty():
            self._pending_resp.get_nowait()

        try:
            while self.running and self.paired:
                data = await asyncio.wait_for(reader.read(1500), timeout=0.5)
                if not data:
                    break

                if not self.el_handshake_done:
                    # Handle elaphureLink handshake locally
                    if len(data) == 12:
                        ident, cmd, ver = struct.unpack('>III', data)
                        if ident == EL_IDENTIFIER and cmd == EL_CMD_HANDSHAKE:
                            resp = struct.pack('>III', EL_IDENTIFIER,
                                               EL_CMD_HANDSHAKE, EL_DAP_VERSION)
                            writer.write(resp)
                            await writer.drain()
                            self.el_handshake_done = True
                            print("  elaphureLink 握手完成")
                            continue
                    print(f"  ✗ 无效的握手数据 (len={len(data)})")
                    break

                # Forward CMSIS-DAP command to ESP32 via relay
                await self.ws.send(bytes([WS_CHANNEL_DAP]) + data)

                # Wait for response from ESP32
                try:
                    resp = await asyncio.wait_for(self._pending_resp.get(), timeout=30.0)
                    writer.write(resp)
                    await writer.drain()
                except asyncio.TimeoutError:
                    print("  ⚠ 等待设备响应超时 (30s)")
                    break

        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ 错误: {e}")
        finally:
            print(f"elaphureLink 已断开: {addr}")
            writer.close()
            self.tcp_writer = None
            self.el_handshake_done = False

    async def handle_serial_client(self, reader: asyncio.StreamReader,
                                   writer: asyncio.StreamWriter):
        """Handle one serial TCP client — bidirectional bridge via relay."""
        addr = writer.get_extra_info('peername')
        print(f"串口桥接已连接: {addr}")
        self.serial_writer = writer

        # Clear stale serial data
        while not self._pending_serial.empty():
            self._pending_serial.get_nowait()

        # Task to forward relay serial → TCP client
        async def serial_relay_to_tcp():
            try:
                while self.running and self.paired:
                    try:
                        data = await asyncio.wait_for(
                            self._pending_serial.get(), timeout=0.5)
                        writer.write(data)
                        await writer.drain()
                    except asyncio.TimeoutError:
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        relay_to_tcp = asyncio.create_task(serial_relay_to_tcp())

        try:
            while self.running and self.paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                # Forward serial data to ESP32 via relay
                await self.ws.send(bytes([WS_CHANNEL_SERIAL]) + data)
        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ 串口错误: {e}")
        finally:
            relay_to_tcp.cancel()
            print(f"串口桥接已断开: {addr}")
            writer.close()
            self.serial_writer = None

    async def handle_rtt_client(self, reader: asyncio.StreamReader,
                                writer: asyncio.StreamWriter):
        """Handle one RTT TCP client — bidirectional bridge via relay."""
        addr = writer.get_extra_info('peername')
        print(f"RTT 已连接: {addr}")
        self.rtt_writer = writer

        while not self._pending_rtt.empty():
            self._pending_rtt.get_nowait()

        async def rtt_relay_to_tcp():
            try:
                while self.running and self.paired:
                    try:
                        data = await asyncio.wait_for(
                            self._pending_rtt.get(), timeout=0.5)
                        writer.write(data)
                        await writer.drain()
                    except asyncio.TimeoutError:
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        relay_to_tcp = asyncio.create_task(rtt_relay_to_tcp())

        try:
            while self.running and self.paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                await self.ws.send(bytes([WS_CHANNEL_RTT]) + data)
        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ RTT 错误: {e}")
        finally:
            relay_to_tcp.cancel()
            print(f"RTT 已断开: {addr}")
            writer.close()
            self.rtt_writer = None

    async def run(self):
        """Main entry point."""
        # Check port availability before connecting to relay
        ports_to_check = [
            (self.tcp_port, "DAP"),
            (self.serial_port, "串口"),
            (self.rtt_port, "RTT"),
        ]
        occupied = []
        for port, name in ports_to_check:
            if not check_port_available(port):
                occupied.append(f"  {name} 端口 {port} 已被占用")
        if occupied:
            print("✗ 以下端口已被占用，请先关闭占用程序后重试：")
            for msg in occupied:
                print(msg)
            return

        # Connect to relay
        if not await self.connect_relay():
            return

        # Start relay reader task
        relay_task = asyncio.create_task(self.relay_reader())

        # Start local TCP servers
        try:
            server = await asyncio.start_server(
                self.handle_el_client, '127.0.0.1', self.tcp_port)
            serial_server = await asyncio.start_server(
                self.handle_serial_client, '127.0.0.1', self.serial_port)
            rtt_server = await asyncio.start_server(
                self.handle_rtt_client, '127.0.0.1', self.rtt_port)
        except OSError as e:
            print(f"✗ 启动本地服务失败: {e}")
            print("  请检查端口是否被其他程序占用，或重启后重试")
            relay_task.cancel()
            if self.ws:
                await self.ws.close()
            return

        print(f"\n=== 桥接就绪 ===")
        print(f"DAP  端口: 127.0.0.1:{self.tcp_port} (elaphureLink)")
        print(f"串口端口: 127.0.0.1:{self.serial_port} (PuTTY/串口工具)")
        print(f"RTT  端口: 127.0.0.1:{self.rtt_port} (PuTTY/串口工具)")
        print(f"按 Ctrl+C 退出\n")

        try:
            await relay_task
        except asyncio.CancelledError:
            pass
        finally:
            server.close()
            serial_server.close()
            rtt_server.close()
            await server.wait_closed()
            await serial_server.wait_closed()
            await rtt_server.wait_closed()
            if self.ws:
                await self.ws.close()


class DAPLinkBridgeTCP:
    """TCP relay bridge — connects to relay-server's TCP port (7001)."""

    def __init__(self, relay_host: str, relay_tcp_port: int, code: str,
                 tcp_port: int = DEFAULT_TCP_PORT,
                 serial_port: int = DEFAULT_SERIAL_PORT,
                 rtt_port: int = DEFAULT_RTT_PORT):
        self.relay_host = relay_host
        self.relay_tcp_port = relay_tcp_port
        self.code = code.upper()
        self.tcp_port = tcp_port
        self.serial_port = serial_port
        self.rtt_port = rtt_port
        self._reader = None
        self._writer = None
        self.tcp_writer = None
        self.serial_writer = None
        self.rtt_writer = None
        self.paired = False
        self.running = True
        self.el_handshake_done = False
        self._pending_resp = asyncio.Queue()
        self._pending_serial = asyncio.Queue()
        self._pending_rtt = asyncio.Queue()

    async def _read_exact(self, n):
        """Read exactly n bytes, return None on disconnect."""
        data = b''
        while len(data) < n:
            chunk = await self._reader.read(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    async def _relay_send(self, data: bytes):
        """Send length-prefixed message to TCP relay."""
        header = struct.pack('>H', len(data))
        self._writer.write(header + data)
        await self._writer.drain()

    async def connect_relay(self):
        """Connect to TCP relay server and register as client."""
        print(f"正在连接 TCP 中继服务器: {self.relay_host}:{self.relay_tcp_port}")
        try:
            self._reader, self._writer = await asyncio.open_connection(
                self.relay_host, self.relay_tcp_port)
        except Exception as e:
            print(f"✗ TCP 连接失败: {e}")
            return False

        # Send registration: [role=0x02(client), code_len, code...]
        code_bytes = self.code.encode('ascii')
        reg_msg = bytes([TCP_ROLE_CLIENT, len(code_bytes)]) + code_bytes
        self._writer.write(reg_msg)
        await self._writer.drain()
        print(f"已注册，配对码: {self.code}，等待设备连接...")

        # Wait for pairing response
        while self.running:
            try:
                status_byte = await asyncio.wait_for(
                    self._read_exact(1), timeout=1.0)
                if not status_byte:
                    print("✗ TCP 中继断开")
                    return False
                status = status_byte[0]
                if status == TCP_STATUS_REGISTERED:
                    continue
                elif status == TCP_STATUS_PAIRED:
                    # [0x01, port_hi, port_lo, ip_len, ip...]
                    rest = await asyncio.wait_for(
                        self._read_exact(3), timeout=5.0)
                    if not rest:
                        self.paired = True
                        print("✓ 设备已连接！")
                        return True
                    peer_port = (rest[0] << 8) | rest[1]
                    ip_len = rest[2]
                    if ip_len > 0:
                        ip_bytes = await asyncio.wait_for(
                            self._read_exact(ip_len), timeout=5.0)
                        if ip_bytes:
                            peer_ip = ip_bytes.decode('ascii', errors='replace')
                            print(f"✓ 设备已连接！(peer: {peer_ip}:{peer_port})")
                    else:
                        print("✓ 设备已连接！")
                    self.paired = True
                    return True
                elif status == TCP_STATUS_PEER_DISCONNECTED:
                    print("✗ 设备已断开")
                    return False
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                print(f"✗ TCP 中继错误: {e}")
                return False
        return False

    async def relay_reader(self):
        """Read length-prefixed messages from TCP relay and dispatch by channel."""
        try:
            while self.running and self.paired:
                # Read 2-byte length header
                try:
                    hdr = await asyncio.wait_for(
                        self._read_exact(2), timeout=5.0)
                except asyncio.TimeoutError:
                    continue
                if hdr is None:
                    print("\n✗ TCP 中继断开")
                    self.paired = False
                    return
                msg_len = (hdr[0] << 8) | hdr[1]
                if msg_len <= 0 or msg_len > 4096:
                    continue

                # Read message body
                try:
                    body = await asyncio.wait_for(
                        self._read_exact(msg_len), timeout=10.0)
                except asyncio.TimeoutError:
                    continue
                if body is None:
                    self.paired = False
                    return

                # Check for disconnect notification
                if msg_len == 1 and body[0] == TCP_STATUS_PEER_DISCONNECTED:
                    print("\n✗ 设备已断开连接")
                    self.paired = False
                    return

                # Dispatch by channel
                if len(body) > 1:
                    channel = body[0]
                    payload = body[1:]
                    if channel == WS_CHANNEL_DAP:
                        await self._pending_resp.put(payload)
                    elif channel == WS_CHANNEL_SERIAL:
                        await self._pending_serial.put(payload)
                    elif channel == WS_CHANNEL_RTT:
                        await self._pending_rtt.put(payload)
                else:
                    # Legacy: no prefix, treat as DAP
                    await self._pending_resp.put(body)
        except (ConnectionResetError, BrokenPipeError, OSError) as e:
            print(f"\n✗ TCP 中继断开: {e}")
            self.paired = False

    async def handle_el_client(self, reader: asyncio.StreamReader,
                               writer: asyncio.StreamWriter):
        """Handle one elaphureLink TCP client connection."""
        addr = writer.get_extra_info('peername')
        print(f"elaphureLink 已连接: {addr}")
        self.tcp_writer = writer
        self.el_handshake_done = False

        while not self._pending_resp.empty():
            self._pending_resp.get_nowait()

        try:
            while self.running and self.paired:
                data = await asyncio.wait_for(reader.read(1500), timeout=0.5)
                if not data:
                    break

                if not self.el_handshake_done:
                    if len(data) == 12:
                        ident, cmd, ver = struct.unpack('>III', data)
                        if ident == EL_IDENTIFIER and cmd == EL_CMD_HANDSHAKE:
                            resp = struct.pack('>III', EL_IDENTIFIER,
                                               EL_CMD_HANDSHAKE, EL_DAP_VERSION)
                            writer.write(resp)
                            await writer.drain()
                            self.el_handshake_done = True
                            print("  elaphureLink 握手完成")
                            continue
                    print(f"  ✗ 无效的握手数据 (len={len(data)})")
                    break

                # Forward CMSIS-DAP command to ESP32 via TCP relay
                await self._relay_send(bytes([WS_CHANNEL_DAP]) + data)

                try:
                    resp = await asyncio.wait_for(self._pending_resp.get(), timeout=30.0)
                    writer.write(resp)
                    await writer.drain()
                except asyncio.TimeoutError:
                    print("  ⚠ 等待设备响应超时 (30s)")
                    break

        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ 错误: {e}")
        finally:
            print(f"elaphureLink 已断开: {addr}")
            writer.close()
            self.tcp_writer = None
            self.el_handshake_done = False

    async def handle_serial_client(self, reader: asyncio.StreamReader,
                                   writer: asyncio.StreamWriter):
        """Handle one serial TCP client — bidirectional bridge via TCP relay."""
        addr = writer.get_extra_info('peername')
        print(f"串口桥接已连接: {addr}")
        self.serial_writer = writer

        while not self._pending_serial.empty():
            self._pending_serial.get_nowait()

        async def serial_relay_to_tcp():
            try:
                while self.running and self.paired:
                    try:
                        data = await asyncio.wait_for(
                            self._pending_serial.get(), timeout=0.5)
                        writer.write(data)
                        await writer.drain()
                    except asyncio.TimeoutError:
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        relay_to_tcp = asyncio.create_task(serial_relay_to_tcp())

        try:
            while self.running and self.paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                await self._relay_send(bytes([WS_CHANNEL_SERIAL]) + data)
        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ 串口错误: {e}")
        finally:
            relay_to_tcp.cancel()
            print(f"串口桥接已断开: {addr}")
            writer.close()
            self.serial_writer = None

    async def handle_rtt_client(self, reader: asyncio.StreamReader,
                                writer: asyncio.StreamWriter):
        """Handle one RTT TCP client — bidirectional bridge via TCP relay."""
        addr = writer.get_extra_info('peername')
        print(f"RTT 已连接: {addr}")
        self.rtt_writer = writer

        while not self._pending_rtt.empty():
            self._pending_rtt.get_nowait()

        async def rtt_relay_to_tcp():
            try:
                while self.running and self.paired:
                    try:
                        data = await asyncio.wait_for(
                            self._pending_rtt.get(), timeout=0.5)
                        writer.write(data)
                        await writer.drain()
                    except asyncio.TimeoutError:
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        relay_to_tcp = asyncio.create_task(rtt_relay_to_tcp())

        try:
            while self.running and self.paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                await self._relay_send(bytes([WS_CHANNEL_RTT]) + data)
        except asyncio.TimeoutError:
            pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            print(f"  ✗ RTT 错误: {e}")
        finally:
            relay_to_tcp.cancel()
            print(f"RTT 已断开: {addr}")
            writer.close()
            self.rtt_writer = None

    async def run(self):
        """Main entry point."""
        ports_to_check = [
            (self.tcp_port, "DAP"),
            (self.serial_port, "串口"),
            (self.rtt_port, "RTT"),
        ]
        occupied = []
        for port, name in ports_to_check:
            if not check_port_available(port):
                occupied.append(f"  {name} 端口 {port} 已被占用")
        if occupied:
            print("✗ 以下端口已被占用，请先关闭占用程序后重试：")
            for msg in occupied:
                print(msg)
            return

        if not await self.connect_relay():
            return

        relay_task = asyncio.create_task(self.relay_reader())

        try:
            server = await asyncio.start_server(
                self.handle_el_client, '127.0.0.1', self.tcp_port)
            serial_server = await asyncio.start_server(
                self.handle_serial_client, '127.0.0.1', self.serial_port)
            rtt_server = await asyncio.start_server(
                self.handle_rtt_client, '127.0.0.1', self.rtt_port)
        except OSError as e:
            print(f"✗ 启动本地服务失败: {e}")
            relay_task.cancel()
            if self._writer:
                self._writer.close()
            return

        print(f"\n=== TCP 中继桥接就绪 ===")
        print(f"DAP  端口: 127.0.0.1:{self.tcp_port} (elaphureLink)")
        print(f"串口端口: 127.0.0.1:{self.serial_port} (PuTTY/串口工具)")
        print(f"RTT  端口: 127.0.0.1:{self.rtt_port} (PuTTY/串口工具)")
        print(f"按 Ctrl+C 退出\n")

        try:
            await relay_task
        except asyncio.CancelledError:
            pass
        finally:
            server.close()
            serial_server.close()
            rtt_server.close()
            await server.wait_closed()
            await serial_server.wait_closed()
            await rtt_server.wait_closed()
            if self._writer:
                self._writer.close()


def load_config(config_path: str) -> dict:
    """Load settings from config file. Returns dict with relay, code, port, serial_port, protocol, relay_tcp_port."""
    cfg = {'relay': '', 'code': '', 'port': DEFAULT_TCP_PORT,
           'serial_port': DEFAULT_SERIAL_PORT, 'rtt_port': DEFAULT_RTT_PORT,
           'protocol': 'ws', 'relay_tcp_port': DEFAULT_RELAY_TCP_PORT}
    if not os.path.exists(config_path):
        return cfg
    parser = configparser.ConfigParser()
    parser.read(config_path, encoding='utf-8')
    if parser.has_section('bridge'):
        cfg['relay'] = parser.get('bridge', 'relay', fallback='').strip()
        cfg['code'] = parser.get('bridge', 'code', fallback='').strip()
        cfg['port'] = parser.getint('bridge', 'port', fallback=DEFAULT_TCP_PORT)
        cfg['serial_port'] = parser.getint('bridge', 'serial_port', fallback=DEFAULT_SERIAL_PORT)
        cfg['rtt_port'] = parser.getint('bridge', 'rtt_port', fallback=DEFAULT_RTT_PORT)
        cfg['protocol'] = parser.get('bridge', 'protocol', fallback='ws').strip()
        cfg['relay_tcp_port'] = parser.getint('bridge', 'relay_tcp_port', fallback=DEFAULT_RELAY_TCP_PORT)
    return cfg


def save_config(config_path: str, relay: str, code: str, port: int,
                serial_port: int, rtt_port: int, protocol: str = 'ws',
                relay_tcp_port: int = DEFAULT_RELAY_TCP_PORT):
    """Save settings to config file for next launch."""
    parser = configparser.ConfigParser()
    parser['bridge'] = {
        'relay': relay,
        'code': code,
        'port': str(port),
        'serial_port': str(serial_port),
        'rtt_port': str(rtt_port),
        'protocol': protocol,
        'relay_tcp_port': str(relay_tcp_port),
    }
    with open(config_path, 'w', encoding='utf-8') as f:
        f.write('# DAPLink PC Bridge 配置文件\n')
        f.write('# 修改后重启桥接工具即可生效\n\n')
        parser.write(f)


def main():
    parser = argparse.ArgumentParser(
        description='DAPLink PC Bridge - 跨网络无线调试桥接工具')
    parser.add_argument('--relay', '-r', type=str,
                        help='中继服务器地址 (WS: ws://server:7000, TCP: server)')
    parser.add_argument('--code', '-c', type=str,
                        help='设备配对码 (6位, 如 A3K7N2)')
    parser.add_argument('--port', '-p', type=int, default=None,
                        help=f'本地 DAP TCP 端口 (默认 {DEFAULT_TCP_PORT})')
    parser.add_argument('--serial-port', '-s', type=int, default=None,
                        help=f'本地串口 TCP 端口 (默认 {DEFAULT_SERIAL_PORT})')
    parser.add_argument('--rtt-port', type=int, default=None,
                        help=f'本地 RTT TCP 端口 (默认 {DEFAULT_RTT_PORT})')
    parser.add_argument('--tcp', action='store_true',
                        help=f'使用 TCP 中继模式 (默认端口 {DEFAULT_RELAY_TCP_PORT})')
    parser.add_argument('--relay-tcp-port', type=int, default=None,
                        help=f'TCP 中继端口 (默认 {DEFAULT_RELAY_TCP_PORT})')
    parser.add_argument('--config', type=str, default=CONFIG_FILE,
                        help=f'配置文件路径 (默认 {CONFIG_FILE})')
    args = parser.parse_args()

    # Load config file
    cfg = load_config(args.config)

    # Priority: command-line args > config file > interactive input
    relay = args.relay or cfg['relay']
    code = args.code or cfg['code']
    port = args.port if args.port is not None else cfg['port']
    serial_port = args.serial_port if args.serial_port is not None else cfg['serial_port']
    rtt_port = args.rtt_port if args.rtt_port is not None else cfg['rtt_port']
    protocol = 'tcp' if args.tcp else cfg['protocol']
    relay_tcp_port = args.relay_tcp_port if args.relay_tcp_port is not None else cfg['relay_tcp_port']

    print("╔══════════════════════════════════╗")
    print("║   DAPLink 无线调试桥接工具       ║")
    print("╚══════════════════════════════════╝")
    print()

    if relay or code:
        mode_str = "TCP" if protocol == "tcp" else "WebSocket"
        print(f"已从配置加载: relay={relay}, code={code}, port={port}, 模式={mode_str}")
        print()

    if not relay:
        relay = input("请输入中继服务器地址 (如 ws://192.168.1.100:7000): ").strip()
        if not relay:
            print("错误: 未输入中继服务器地址")
            return

    if not code:
        code = input("请输入设备配对码 (6位): ").strip()
        if not code:
            print("错误: 未输入配对码")
            return

    # Determine protocol from URL
    if protocol != 'tcp':
        if not relay.startswith('ws://') and not relay.startswith('wss://'):
            relay = 'ws://' + relay

    # Save to config for next launch
    save_config(args.config, relay, code, port, serial_port, rtt_port,
                protocol, relay_tcp_port)
    print(f"配置已保存到: {args.config}")
    print()

    if protocol == 'tcp':
        # Extract host from relay URL (strip ws:// prefix if present)
        relay_host = relay.replace('ws://', '').replace('wss://', '')
        # Remove port if present (user may give host:port)
        if ':' in relay_host:
            parts = relay_host.rsplit(':', 1)
            relay_host = parts[0]
            if relay_tcp_port == DEFAULT_RELAY_TCP_PORT:
                try:
                    relay_tcp_port = int(parts[1])
                except ValueError:
                    pass
        bridge = DAPLinkBridgeTCP(relay_host, relay_tcp_port, code,
                                  port, serial_port, rtt_port)
    else:
        bridge = DAPLinkBridge(relay, code, port, serial_port, rtt_port)

    # Handle Ctrl+C
    def signal_handler(sig, frame):
        bridge.running = False
        print("\n正在退出...")

    signal.signal(signal.SIGINT, signal_handler)

    asyncio.run(bridge.run())


if __name__ == '__main__':
    main()
