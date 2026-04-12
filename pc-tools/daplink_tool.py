"""
DAPLink Tool — 集成 PC 端工具
功能: 串口终端 | 自定义协议收发 | WiFi 桥接 (直连 + 中继) | RTT 查看器 | 配置管理
打包: pyinstaller --onefile --windowed daplink_tool.py
"""

import asyncio
import configparser
import ctypes
import ctypes.wintypes
import glob
import json
import os
import queue
import struct
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse
import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
import xml.etree.ElementTree as ET

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

try:
    import websockets
except ImportError:
    websockets = None

try:
    from el_proxy import ElProxy
except ImportError:
    ElProxy = None


def _find_elaphurelink_exe():
    """Auto-find elaphureLink.Wpf.exe relative to the app."""
    if getattr(sys, 'frozen', False):
        base = sys._MEIPASS  # PyInstaller temp dir
    else:
        base = os.path.dirname(os.path.abspath(__file__))

    # Check common locations
    candidates = [
        os.path.join(base, 'elaphureLink_Windows_x64_release', 'elaphureLink.Wpf.exe'),
        os.path.join(base, 'elaphureLink.Wpf.exe'),
        os.path.join(os.path.dirname(base), 'elaphureLink_Windows_x64_release', 'elaphureLink.Wpf.exe'),
    ]
    for p in candidates:
        if os.path.isfile(p):
            return p
    return ""


def _set_elaphurelink_device_address(address: str):
    """Update deviceAddress in all elaphureLink user.config files."""
    base = os.path.join(os.environ.get("LOCALAPPDATA", ""), "elaphureLink")
    if not os.path.isdir(base):
        return
    for cfg_path in glob.glob(os.path.join(base, "**", "user.config"), recursive=True):
        try:
            tree = ET.parse(cfg_path)
            root = tree.getroot()
            for setting in root.iter("setting"):
                if setting.get("name") == "deviceAddress":
                    val = setting.find("value")
                    if val is not None:
                        val.text = address
            tree.write(cfg_path, encoding="utf-8", xml_declaration=True)
        except Exception:
            pass


def _find_keil_install_dir():
    """Find Keil MDK installation directory from registry or common paths."""
    import winreg
    # Try registry first
    for key_path in [
        r"SOFTWARE\Keil\Products\MDK",
        r"SOFTWARE\WOW6432Node\Keil\Products\MDK",
        r"SOFTWARE\Keil\Products\µVision",
        r"SOFTWARE\WOW6432Node\Keil\Products\µVision",
    ]:
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path) as key:
                path, _ = winreg.QueryValueEx(key, "Path")
                if os.path.isdir(path):
                    return path
        except (OSError, FileNotFoundError):
            pass
    # Fallback: common installation paths
    for p in [r"C:\Keil_v5", r"D:\Keil_v5", r"C:\Keil", r"D:\Keil",
              r"E:\Keil_v5", r"E:\Keil"]:
        if os.path.isdir(p):
            return p
    return ""


def _find_tools_ini(keil_dir: str):
    """Find TOOLS.INI in Keil installation (searches current dir and parents)."""
    # Search in the given dir and up to 2 parent levels
    search_dirs = [keil_dir]
    parent = os.path.dirname(keil_dir)
    if parent and parent != keil_dir:
        search_dirs.append(parent)
        grandparent = os.path.dirname(parent)
        if grandparent and grandparent != parent:
            search_dirs.append(grandparent)

    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if f.upper() == "TOOLS.INI":
                return os.path.join(d, f)
    return ""


def _install_keil_elaphurelink_driver(el_release_dir: str, log_callback=None,
                                      tools_ini_override: str = ""):
    """Install elaphureLink RDDI driver into Keil.

    Steps:
    1. Find Keil installation
    2. Copy elaphureRddi.dll to ARM/BIN
    3. Register in TOOLS.INI

    Returns (success: bool, message: str).
    """
    log = log_callback or (lambda m: None)

    # Check if elaphureRddi.dll exists in release dir
    rddi_src = os.path.join(el_release_dir, "elaphureRddi.dll")
    if not os.path.isfile(rddi_src):
        return False, f"未找到 elaphureRddi.dll: {el_release_dir}"

    tools_ini = tools_ini_override
    if not tools_ini:
        keil_dir = _find_keil_install_dir()
        if not keil_dir:
            return False, "未找到 Keil 安装目录\n请手动选择 TOOLS.INI 文件"
        tools_ini = _find_tools_ini(keil_dir)
        if not tools_ini:
            return False, f"未找到 TOOLS.INI: {keil_dir}\n请手动选择 TOOLS.INI 文件"

    log(f"使用 TOOLS.INI: {tools_ini}")

    # Determine Keil root dir from TOOLS.INI location
    keil_root = os.path.dirname(tools_ini)

    # Find ARM/BIN directory
    arm_bin = os.path.join(keil_root, "ARM", "BIN")
    if not os.path.isdir(arm_bin):
        # Maybe we're already inside ARM
        candidate = os.path.join(keil_root, "BIN")
        if os.path.isdir(candidate):
            arm_bin = candidate
        else:
            os.makedirs(arm_bin, exist_ok=True)

    rddi_dst = os.path.join(arm_bin, "elaphureRddi.dll")

    # Copy DLL
    import shutil
    try:
        shutil.copy2(rddi_src, rddi_dst)
        log(f"已复制 elaphureRddi.dll -> {rddi_dst}")
    except PermissionError:
        return False, f"复制失败(权限不足): {rddi_dst}\n请以管理员身份运行"
    except Exception as e:
        return False, f"复制失败: {e}"

    # Also copy elaphureLink.dll if present
    el_dll_src = os.path.join(el_release_dir, "elaphureLink.dll")
    if os.path.isfile(el_dll_src):
        try:
            shutil.copy2(el_dll_src, os.path.join(arm_bin, "elaphureLink.dll"))
        except Exception:
            pass

    # Register in TOOLS.INI - check if already registered
    try:
        with open(tools_ini, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        return False, f"读取 TOOLS.INI 失败: {e}"

    if "elaphureLink" in content:
        log("TOOLS.INI 中已有 elaphureLink 配置")
        return True, f"安装完成 (已存在)\nKeil目录: {keil_root}"

    # Find [ARM] section and add RDDI driver entry
    # elaphureLink registers as: TDRV9=BIN\elaphureRddi.dll("elaphureLink")
    # Find the highest TDRVx number
    import re
    tdrv_pattern = re.compile(r'TDRV(\d+)\s*=', re.IGNORECASE)
    max_tdrv = 0
    for m in tdrv_pattern.finditer(content):
        num = int(m.group(1))
        if num > max_tdrv:
            max_tdrv = num

    new_tdrv = max_tdrv + 1
    tdrv_line = f'TDRV{new_tdrv}=BIN\\elaphureRddi.dll("elaphureLink")\n'

    # Insert after [ARM] section header, or after last TDRV line
    lines = content.split('\n')
    insert_idx = -1
    for i, line in enumerate(lines):
        if tdrv_pattern.search(line):
            insert_idx = i + 1  # After last TDRV line

    if insert_idx < 0:
        # No TDRV entries found; look for [ARM] section
        for i, line in enumerate(lines):
            if line.strip().upper() == '[ARM]':
                insert_idx = i + 1
                break

    if insert_idx < 0:
        return False, "TOOLS.INI 中未找到 [ARM] 段或 TDRV 配置"

    lines.insert(insert_idx, tdrv_line.rstrip())

    try:
        with open(tools_ini, "w", encoding="utf-8") as f:
            f.write('\n'.join(lines))
        log(f"已写入 TOOLS.INI: {tdrv_line.strip()}")
    except PermissionError:
        return False, f"写入 TOOLS.INI 失败(权限不足)\n请以管理员身份运行"
    except Exception as e:
        return False, f"写入 TOOLS.INI 失败: {e}"

    return True, f"安装完成\nKeil目录: {keil_root}\n驱动: TDRV{new_tdrv}"


# ─── Win32 helpers for embedding external windows ────────────────────────────

if sys.platform == "win32":
    _user32 = ctypes.windll.user32
    _GWL_STYLE = -16
    _WS_CHILD = 0x40000000
    _WS_VISIBLE = 0x10000000
    _SWP_FRAMECHANGED = 0x0020
    _WS_CAPTION = 0x00C00000
    _WS_THICKFRAME = 0x00040000
    _WS_MINIMIZEBOX = 0x00020000
    _WS_MAXIMIZEBOX = 0x00010000
    _WS_SYSMENU = 0x00080000

    _EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)

    def _find_hwnd_by_pid(pid: int, timeout: float = 5.0) -> int:
        """Find the main window handle belonging to *pid*. Retries for *timeout* seconds."""
        import time as _t
        deadline = _t.monotonic() + timeout
        while _t.monotonic() < deadline:
            result = []
            def _cb(hwnd, _):
                tid_pid = ctypes.wintypes.DWORD()
                _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(tid_pid))
                if tid_pid.value == pid and _user32.IsWindowVisible(hwnd):
                    result.append(hwnd)
                return True
            _user32.EnumWindows(_EnumWindowsProc(_cb), 0)
            if result:
                return result[0]
            _t.sleep(0.3)
        return 0

    def _embed_hwnd(hwnd: int, parent_hwnd: int):
        """Reparent *hwnd* into *parent_hwnd* and strip window chrome."""
        style = _user32.GetWindowLongW(hwnd, _GWL_STYLE)
        style = (style & ~(_WS_CAPTION | _WS_THICKFRAME | _WS_MINIMIZEBOX |
                           _WS_MAXIMIZEBOX | _WS_SYSMENU)) | _WS_CHILD | _WS_VISIBLE
        _user32.SetWindowLongW(hwnd, _GWL_STYLE, style)
        _user32.SetParent(hwnd, parent_hwnd)
        _user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, _SWP_FRAMECHANGED | 0x0003)

    def _resize_embedded(hwnd: int, w: int, h: int):
        """Move embedded window to fill parent area."""
        _user32.MoveWindow(hwnd, 0, 0, w, h, True)

# ─── Constants ────────────────────────────────────────────────────────────────

APP_VERSION = "1.0.0"
APP_TITLE = f"DAPLink Tool v{APP_VERSION}"
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config.ini")

EL_IDENTIFIER = 0x8A656C70
EL_CMD_HANDSHAKE = 0x00000000
EL_DAP_VERSION = 0x00000001

DEFAULT_DAP_PORT = 3240
DEFAULT_SERIAL_PORT = 3241
DEFAULT_RTT_PORT = 3242
DEFAULT_RELAY_PORT = 7000

WS_CHANNEL_DAP = 0x00
WS_CHANNEL_SERIAL = 0x01
WS_CHANNEL_RTT = 0x02
WS_CHANNEL_FLASH = 0x03
WS_CHANNEL_PUNCH = 0x04

PUNCH_MY_PORT = 0x01
PUNCH_DIRECT = 0x02
PUNCH_MAGIC = b"DAPLINK_PUNCH"
PUNCH_ACK = b"DAPLINK_PUNCH_ACK"
STUN_PORT = 7002

FLASH_CMD_START = 0x01
FLASH_CMD_DATA = 0x02
FLASH_CMD_FINISH = 0x03

FLASH_STATUS_OK = 0x00
FLASH_STATUS_ERROR = 0x01
FLASH_STATUS_PROGRESS = 0x02

BAUD_RATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]

# ─── CRC / Checksum ──────────────────────────────────────────────────────────

def _crc_sum8(data: bytes) -> bytes:
    """累加和校验 (取低字节)"""
    return bytes([sum(data) & 0xFF])

def _crc_sum16(data: bytes) -> bytes:
    """累加和校验 (取低2字节, LE)"""
    return struct.pack('<H', sum(data) & 0xFFFF)

def _crc_xor8(data: bytes) -> bytes:
    """异或校验"""
    v = 0
    for b in data:
        v ^= b
    return bytes([v & 0xFF])

def _crc16_modbus(data: bytes) -> bytes:
    """CRC-16/MODBUS (LE)"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return struct.pack('<H', crc)

def _crc16_ccitt(data: bytes) -> bytes:
    """CRC-16/CCITT-FALSE (BE)"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return struct.pack('>H', crc)

def _crc32_calc(data: bytes) -> bytes:
    """CRC-32 (LE)"""
    import binascii
    return struct.pack('<I', binascii.crc32(data) & 0xFFFFFFFF)

CRC_METHODS = {
    "SUM8":       ("累加和(1B)",       _crc_sum8),
    "SUM16":      ("累加和(2B LE)",    _crc_sum16),
    "XOR8":       ("异或(1B)",         _crc_xor8),
    "CRC16_MOD":  ("CRC16/MODBUS(2B)", _crc16_modbus),
    "CRC16_CCITT":("CRC16/CCITT(2B)",  _crc16_ccitt),
    "CRC32":      ("CRC32(4B)",        _crc32_calc),
}

# Protocol field types
FIELD_CONST   = "const"    # 固定字节
FIELD_LENGTH  = "length"   # 自动长度
FIELD_DATA    = "data"     # 可编辑数据
FIELD_CRC     = "crc"      # 自动校验

FIELD_TYPE_LABELS = {
    FIELD_CONST:  "固定",
    FIELD_LENGTH: "长度",
    FIELD_DATA:   "数据",
    FIELD_CRC:    "校验",
}

# Receive parse data types
RX_DTYPE_UINT8  = "uint8"
RX_DTYPE_INT8   = "int8"
RX_DTYPE_UINT16_LE = "uint16_le"
RX_DTYPE_UINT16_BE = "uint16_be"
RX_DTYPE_INT16_LE  = "int16_le"
RX_DTYPE_INT16_BE  = "int16_be"
RX_DTYPE_UINT32_LE = "uint32_le"
RX_DTYPE_UINT32_BE = "uint32_be"
RX_DTYPE_INT32_LE  = "int32_le"
RX_DTYPE_INT32_BE  = "int32_be"
RX_DTYPE_FLOAT_LE  = "float_le"
RX_DTYPE_FLOAT_BE  = "float_be"
RX_DTYPE_RAW       = "raw"

RX_DTYPE_INFO = {
    RX_DTYPE_UINT8:     ("uint8",     1, "<B"),
    RX_DTYPE_INT8:      ("int8",      1, "<b"),
    RX_DTYPE_UINT16_LE: ("uint16 LE", 2, "<H"),
    RX_DTYPE_UINT16_BE: ("uint16 BE", 2, ">H"),
    RX_DTYPE_INT16_LE:  ("int16 LE",  2, "<h"),
    RX_DTYPE_INT16_BE:  ("int16 BE",  2, ">h"),
    RX_DTYPE_UINT32_LE: ("uint32 LE", 4, "<I"),
    RX_DTYPE_UINT32_BE: ("uint32 BE", 4, ">I"),
    RX_DTYPE_INT32_LE:  ("int32 LE",  4, "<i"),
    RX_DTYPE_INT32_BE:  ("int32 BE",  4, ">i"),
    RX_DTYPE_FLOAT_LE:  ("float LE",  4, "<f"),
    RX_DTYPE_FLOAT_BE:  ("float BE",  4, ">f"),
    RX_DTYPE_RAW:       ("raw",       0, None),
}

def _default_field(name="字段", ftype=FIELD_DATA, hex_val="00"):
    """Create a default protocol field dict."""
    return {
        "name": name,
        "type": ftype,
        "hex": hex_val,
        "data_type": "raw",       # raw, uint8, int8, uint16, int16, uint32, int32, float
        "data_endian": "le",      # le or be
        "data_value": "",         # decimal value (for typed data)
        "length_cover": "all",    # "all" or comma-separated field indices (0-based)
        "length_size": 1,         # 1 or 2 bytes
        "length_endian": "le",
        "crc_method": "SUM8",
        "crc_cover": "all",       # "all" or comma-separated field indices (0-based)
    }

def _default_protocol():
    """Return a default example protocol definition."""
    return [
        _default_field("帧头", FIELD_CONST, "AA55"),
        _default_field("长度", FIELD_LENGTH, ""),
        _default_field("命令", FIELD_DATA, "01"),
        _default_field("数据", FIELD_DATA, "0000"),
        _default_field("校验", FIELD_CRC, ""),
    ]

def _parse_cover_indices(cover_str, total_fields, self_idx, include_self=True):
    """Parse cover string to list of field indices.
    'all' => all fields (include_self controls whether self is included).
    '1,2,3' => specific 0-based indices.
    """
    if cover_str.strip().lower() == "all":
        if include_self:
            return list(range(total_fields))
        else:
            return [i for i in range(total_fields) if i != self_idx]
    indices = []
    for part in cover_str.split(","):
        part = part.strip()
        if part.isdigit():
            idx = int(part)
            if 0 <= idx < total_fields:
                indices.append(idx)
    return indices

def _assemble_protocol(fields):
    """Assemble a protocol frame from field definitions.
    Returns (assembled_bytes, field_byte_ranges) where field_byte_ranges
    is a list of (start, end) byte offsets for each field.
    """
    n = len(fields)
    # First pass: compute raw bytes for const and data fields
    raw_parts = [None] * n
    for i, f in enumerate(fields):
        if f["type"] == FIELD_CONST:
            try:
                raw_parts[i] = bytes.fromhex(f["hex"].replace(" ", ""))
            except ValueError:
                raw_parts[i] = b""
        elif f["type"] == FIELD_DATA:
            dtype = f.get("data_type", "raw")
            endian = f.get("data_endian", "le")
            dec_val = f.get("data_value", "").strip()
            if dtype != "raw" and dec_val:
                try:
                    e = '<' if endian == 'le' else '>'
                    if dtype == "uint8":
                        raw_parts[i] = struct.pack(f"{e}B", int(dec_val) & 0xFF)
                    elif dtype == "int8":
                        raw_parts[i] = struct.pack(f"{e}b", int(dec_val))
                    elif dtype == "uint16":
                        raw_parts[i] = struct.pack(f"{e}H", int(dec_val) & 0xFFFF)
                    elif dtype == "int16":
                        raw_parts[i] = struct.pack(f"{e}h", int(dec_val))
                    elif dtype == "uint32":
                        raw_parts[i] = struct.pack(f"{e}I", int(dec_val) & 0xFFFFFFFF)
                    elif dtype == "int32":
                        raw_parts[i] = struct.pack(f"{e}i", int(dec_val))
                    elif dtype == "float":
                        raw_parts[i] = struct.pack(f"{e}f", float(dec_val))
                    else:
                        raw_parts[i] = bytes.fromhex(f["hex"].replace(" ", ""))
                except (ValueError, struct.error):
                    raw_parts[i] = bytes.fromhex(f.get("hex", "00").replace(" ", "")) if f.get("hex") else b"\x00"
            else:
                try:
                    raw_parts[i] = bytes.fromhex(f["hex"].replace(" ", ""))
                except ValueError:
                    raw_parts[i] = b""
        elif f["type"] == FIELD_LENGTH:
            raw_parts[i] = b"\x00" * f.get("length_size", 1)
        elif f["type"] == FIELD_CRC:
            method_key = f.get("crc_method", "SUM8")
            if method_key in CRC_METHODS:
                _, func = CRC_METHODS[method_key]
                crc_size = len(func(b"\x00"))
            else:
                crc_size = 1
            raw_parts[i] = b"\x00" * crc_size

    # Second pass: compute length fields
    for i, f in enumerate(fields):
        if f["type"] == FIELD_LENGTH:
            cover = _parse_cover_indices(f.get("length_cover", "all"), n, i, include_self=True)
            total_len = sum(len(raw_parts[j]) for j in cover if raw_parts[j] is not None)
            size = f.get("length_size", 1)
            endian = f.get("length_endian", "le")
            if size == 1:
                raw_parts[i] = bytes([total_len & 0xFF])
            elif size == 2:
                fmt = '<H' if endian == "le" else '>H'
                raw_parts[i] = struct.pack(fmt, total_len & 0xFFFF)
            else:
                raw_parts[i] = bytes([total_len & 0xFF])

    # Third pass: compute CRC fields
    for i, f in enumerate(fields):
        if f["type"] == FIELD_CRC:
            cover = _parse_cover_indices(f.get("crc_cover", "all"), n, i, include_self=False)
            covered_data = b""
            for j in cover:
                if raw_parts[j] is not None:
                    covered_data += raw_parts[j]
            method_key = f.get("crc_method", "SUM8")
            if method_key in CRC_METHODS:
                _, func = CRC_METHODS[method_key]
                raw_parts[i] = func(covered_data)
            else:
                raw_parts[i] = _crc_sum8(covered_data)

    # Assemble
    result = b""
    ranges = []
    for part in raw_parts:
        start = len(result)
        if part:
            result += part
        ranges.append((start, len(result)))

    return result, ranges

# ─── Config ───────────────────────────────────────────────────────────────────

class AppConfig:
    def __init__(self, path=CONFIG_FILE):
        self.path = path
        self.serial_port = ""
        self.serial_baud = 115200
        self.terminal_mode = "serial"  # "serial" or "tcp_client"
        self.tcp_client_host = ""
        self.tcp_client_port = 3241
        self.mode = "relay"  # "direct" or "relay"
        self.esp_ip = ""
        self.relay_url = ""
        self.relay_host = ""
        self.relay_port = DEFAULT_RELAY_PORT
        self.relay_code = ""
        self.relay_protocol = "ws"  # "ws" or "tcp"
        self.relay_tcp_port = 7001
        self.dap_port = DEFAULT_DAP_PORT
        self.serial_tcp_port = DEFAULT_SERIAL_PORT
        self.rtt_tcp_port = DEFAULT_RTT_PORT
        self.esp_port = DEFAULT_DAP_PORT
        self.quick_commands = ["status", "help", "reset"]
        self.elaphurelink_path = ""
        self.elaphurelink_autostart = False
        self.elaphurelink_mode = "external"  # "external" or "integrated"
        # Protocol (hex custom protocol)
        self.protocol_fields = _default_protocol()
        self.protocol_list = []  # list of {"name": str, "direction": str, "fields": list}
        self.rx_protocol_enabled = False
        self.rx_header_hex = "AA55"
        self.rx_crc_method = "SUM8"
        self.rx_fields = []  # list of {"name": str, "dtype": str, "size": int}
        self.load()

    def load(self):
        if not os.path.exists(self.path):
            return
        p = configparser.ConfigParser()
        p.read(self.path, encoding="utf-8")
        if p.has_section("serial"):
            self.serial_port = p.get("serial", "port", fallback="")
            self.serial_baud = p.getint("serial", "baud", fallback=115200)
            self.terminal_mode = p.get("serial", "terminal_mode", fallback="serial")
            self.tcp_client_host = p.get("serial", "tcp_client_host", fallback="")
            self.tcp_client_port = p.getint("serial", "tcp_client_port", fallback=3241)
        if p.has_section("bridge"):
            self.mode = p.get("bridge", "mode", fallback="relay")
            self.esp_ip = p.get("bridge", "esp_ip", fallback="")
            self.relay_url = p.get("bridge", "relay_url", fallback="")
            self.relay_host = p.get("bridge", "relay_host", fallback="")
            self.relay_port = p.getint("bridge", "relay_port", fallback=DEFAULT_RELAY_PORT)
            self.relay_code = p.get("bridge", "relay_code", fallback="")
            self.relay_protocol = p.get("bridge", "relay_protocol", fallback="ws")
            self.relay_tcp_port = p.getint("bridge", "relay_tcp_port", fallback=7001)
            self.dap_port = p.getint("bridge", "dap_port", fallback=DEFAULT_DAP_PORT)
            self.serial_tcp_port = p.getint("bridge", "serial_tcp_port", fallback=DEFAULT_SERIAL_PORT)
            self.rtt_tcp_port = p.getint("bridge", "rtt_tcp_port", fallback=DEFAULT_RTT_PORT)
            self.esp_port = p.getint("bridge", "esp_port", fallback=DEFAULT_DAP_PORT)

            # Backward-compatible parsing for old relay_url-only config.
            if not self.relay_host and self.relay_url:
                try:
                    parsed = urlparse(self.relay_url if "://" in self.relay_url else f"ws://{self.relay_url}")
                    self.relay_host = parsed.hostname or ""
                    if parsed.port:
                        self.relay_port = parsed.port
                except Exception:
                    pass
        if p.has_section("commands"):
            raw = p.get("commands", "quick", fallback="[]")
            try:
                parsed = json.loads(raw)
                if isinstance(parsed, list):
                    self.quick_commands = [str(x) for x in parsed if str(x).strip()]
            except Exception:
                pass
        if p.has_section("elaphurelink"):
            self.elaphurelink_path = p.get("elaphurelink", "path", fallback="")
            self.elaphurelink_autostart = p.getboolean("elaphurelink", "autostart", fallback=False)
            self.elaphurelink_mode = p.get("elaphurelink", "mode", fallback="external")
        if p.has_section("protocol"):
            raw = p.get("protocol", "fields", fallback="")
            if raw:
                try:
                    parsed = json.loads(raw)
                    if isinstance(parsed, list) and parsed:
                        self.protocol_fields = parsed
                except Exception:
                    pass
            raw_list = p.get("protocol", "protocol_list", fallback="")
            if raw_list:
                try:
                    parsed_list = json.loads(raw_list)
                    if isinstance(parsed_list, list):
                        self.protocol_list = parsed_list
                except Exception:
                    pass
            self.rx_protocol_enabled = p.getboolean("protocol", "rx_enabled", fallback=False)
            self.rx_header_hex = p.get("protocol", "rx_header", fallback="AA55")
            self.rx_crc_method = p.get("protocol", "rx_crc_method", fallback="SUM8")
            raw_rx = p.get("protocol", "rx_fields", fallback="")
            if raw_rx:
                try:
                    parsed_rx = json.loads(raw_rx)
                    if isinstance(parsed_rx, list):
                        self.rx_fields = parsed_rx
                except Exception:
                    pass

    def save(self):
        p = configparser.ConfigParser()
        p["serial"] = {
            "port": self.serial_port,
            "baud": str(self.serial_baud),
            "terminal_mode": self.terminal_mode,
            "tcp_client_host": self.tcp_client_host,
            "tcp_client_port": str(self.tcp_client_port),
        }
        p["bridge"] = {
            "mode": self.mode,
            "esp_ip": self.esp_ip,
            "relay_url": self.relay_url,
            "relay_host": self.relay_host,
            "relay_port": str(self.relay_port),
            "relay_code": self.relay_code,
            "relay_protocol": self.relay_protocol,
            "relay_tcp_port": str(self.relay_tcp_port),
            "dap_port": str(self.dap_port),
            "serial_tcp_port": str(self.serial_tcp_port),
            "rtt_tcp_port": str(self.rtt_tcp_port),
            "esp_port": str(self.esp_port),
        }
        p["commands"] = {
            "quick": json.dumps(self.quick_commands, ensure_ascii=False),
        }
        p["elaphurelink"] = {
            "path": self.elaphurelink_path,
            "autostart": str(self.elaphurelink_autostart),
            "mode": self.elaphurelink_mode,
        }
        p["protocol"] = {
            "fields": json.dumps(self.protocol_fields, ensure_ascii=False),
            "protocol_list": json.dumps(self.protocol_list, ensure_ascii=False),
            "rx_enabled": str(self.rx_protocol_enabled),
            "rx_header": self.rx_header_hex,
            "rx_crc_method": self.rx_crc_method,
            "rx_fields": json.dumps(self.rx_fields, ensure_ascii=False),
        }
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("# DAPLink Tool 配置文件\n\n")
            p.write(f)


def parse_intel_hex(path: str):
    """Parse Intel HEX file. Returns list of (address, bytes) segments, merged and sorted."""
    segments = []
    base_address = 0
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                continue
            raw = bytes.fromhex(line[1:])
            byte_count = raw[0]
            offset = (raw[1] << 8) | raw[2]
            record_type = raw[3]
            data = raw[4:4 + byte_count]
            if record_type == 0x00:
                addr = base_address + offset
                segments.append((addr, data))
            elif record_type == 0x01:
                break
            elif record_type == 0x04 and byte_count == 2:
                base_address = (data[0] << 24) | (data[1] << 16)

    segments.sort(key=lambda s: s[0])

    # Merge contiguous segments
    merged = []
    for addr, data in segments:
        if merged and addr == merged[-1][0] + len(merged[-1][1]):
            merged[-1] = (merged[-1][0], merged[-1][1] + data)
        else:
            merged.append((addr, bytes(data)))
    return merged


def parse_el_handshake(data: bytes):
    """Return detected endian ('>' or '<') for a valid elaphureLink handshake."""
    if len(data) != 12:
        return None
    try:
        ident, cmd, _ = struct.unpack(">III", data)
        if ident == EL_IDENTIFIER and cmd == EL_CMD_HANDSHAKE:
            return ">"
    except struct.error:
        return None
    try:
        ident, cmd, _ = struct.unpack("<III", data)
        if ident == EL_IDENTIFIER and cmd == EL_CMD_HANDSHAKE:
            return "<"
    except struct.error:
        return None
    return None


def build_el_handshake_response(endian: str = ">") -> bytes:
    return struct.pack(f"{endian}III", EL_IDENTIFIER, EL_CMD_HANDSHAKE, EL_DAP_VERSION)

# ─── Bridge Engine (runs in background thread with asyncio) ───────────────────

class BridgeEngine:
    """Handles both direct TCP and relay WebSocket connections."""

    def __init__(self, log_queue: queue.Queue, serial_rx_queue: queue.Queue,
                 rtt_rx_queue: queue.Queue):
        self._log_q = log_queue
        self._serial_rx_q = serial_rx_queue
        self._rtt_rx_q = rtt_rx_queue
        self._loop: asyncio.AbstractEventLoop | None = None
        self._thread: threading.Thread | None = None
        self._running = False
        self._ws = None
        self._paired = False
        self._mode = "relay"
        self._relay_protocol = "ws"
        self._el_handshake_done = False
        # TCP relay state
        self._tcp_reader = None
        self._tcp_writer = None
        self._pending_dap = asyncio.Queue()
        self._pending_serial = asyncio.Queue()
        self._pending_rtt = asyncio.Queue()
        self._pending_flash = asyncio.Queue()
        # NAT hole punch state
        self._punch_udp_transport = None
        self._punch_udp_protocol = None
        self._punch_peer_ip = None
        self._punch_peer_port = 0
        self._punch_peer_udp_port = 0
        self._punch_success = False
        self._punch_dap_active = False
        self._punch_direct_addr = None  # (ip, port) of peer after successful punch
        # DAP_Info response cache (saves round-trips for repeated queries)
        self._dap_info_cache = {}
        # Pipeline depth: number of fire-and-forget commands whose responses
        # should be silently discarded when they arrive
        self._pipeline_depth = 0
        # For direct mode
        self._direct_reader = None
        self._direct_writer = None

    @property
    def is_running(self):
        return self._running

    def log(self, msg):
        self._log_q.put(msg)

    def start(self, mode, relay_url="", relay_host="", relay_port=DEFAULT_RELAY_PORT,
              code="", esp_ip="", esp_port=DEFAULT_DAP_PORT,
              dap_port=DEFAULT_DAP_PORT, serial_port=DEFAULT_SERIAL_PORT,
              rtt_port=DEFAULT_RTT_PORT, relay_protocol="ws", relay_tcp_port=7001):
        if self._running:
            return
        self._mode = mode
        self._relay_protocol = relay_protocol
        self._running = True
        # Recreate asyncio Queues for the new event loop
        self._pending_dap = asyncio.Queue()
        self._pending_flash = asyncio.Queue()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(
            target=self._run_loop,
            args=(relay_url, relay_host, relay_port, code, esp_ip, esp_port,
                  dap_port, serial_port, rtt_port, relay_protocol, relay_tcp_port),
            daemon=True,
        )
        self._thread.start()

    def stop(self):
        self._running = False
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(self._loop.stop)

    def send_serial(self, data: bytes):
        if self._loop and self._running:
            asyncio.run_coroutine_threadsafe(self._send_serial(data), self._loop)

    def send_rtt(self, data: bytes):
        if self._loop and self._running:
            asyncio.run_coroutine_threadsafe(self._send_rtt(data), self._loop)

    async def _relay_send(self, data: bytes):
        """Send channel-prefixed data via WS or TCP relay."""
        try:
            if self._relay_protocol == "tcp" and self._tcp_writer:
                header = struct.pack('>H', len(data))
                self._tcp_writer.write(header + data)
                await self._tcp_writer.drain()
            elif self._ws:
                await self._ws.send(data)
        except Exception:
            pass

    async def _send_serial(self, data: bytes):
        try:
            if self._mode == "relay" and self._paired:
                await self._relay_send(bytes([WS_CHANNEL_SERIAL]) + data)
            elif self._mode == "direct" and self._direct_writer:
                pass  # Direct mode serial: not yet implemented
        except Exception:
            pass

    async def _send_rtt(self, data: bytes):
        try:
            if self._mode == "relay" and self._paired:
                await self._relay_send(bytes([WS_CHANNEL_RTT]) + data)
        except Exception:
            pass

    def remote_flash(self, hex_path: str, log_callback=None):
        """Start remote flash in bridge thread. log_callback(msg) for progress."""
        if not self._loop or not self._running:
            if log_callback:
                log_callback("桥接未运行")
            return
        asyncio.run_coroutine_threadsafe(
            self._remote_flash(hex_path, log_callback), self._loop)

    async def _remote_flash(self, hex_path: str, log_cb):
        """Parse hex file and upload to ESP32 for local programming."""
        def log(msg):
            self.log(msg)
            if log_cb:
                try:
                    log_cb(msg)
                except Exception:
                    pass

        if not self._paired:
            log("远程烧录: 设备未连接")
            return

        # Parse hex file
        try:
            segments = parse_intel_hex(hex_path)
        except Exception as e:
            log(f"远程烧录: 解析 HEX 失败: {e}")
            return

        total_bytes = sum(len(s[1]) for s in segments)
        log(f"远程烧录: {len(segments)} 段, 共 {total_bytes} 字节")

        # Clear pending flash responses
        while not self._pending_flash.empty():
            self._pending_flash.get_nowait()

        # Step 1: Send START
        await self._relay_send(bytes([WS_CHANNEL_FLASH, FLASH_CMD_START]))
        resp = await self._wait_flash_resp(10)
        if not resp or resp[0] != FLASH_STATUS_OK:
            log(f"远程烧录: 启动失败: {self._flash_resp_msg(resp)}")
            return
        log("远程烧录: 设备就绪")

        # Step 2: Send DATA segments in chunks
        chunk_size = 1024  # 1KB per WS message
        sent = 0
        for addr, data in segments:
            offset = 0
            while offset < len(data):
                chunk = data[offset:offset + chunk_size]
                cur_addr = addr + offset
                # [channel, cmd, addr_le(4), data...]
                msg = bytes([WS_CHANNEL_FLASH, FLASH_CMD_DATA])
                msg += struct.pack('<I', cur_addr) + chunk
                await self._relay_send(msg)
                resp = await self._wait_flash_resp(10)
                if not resp or resp[0] != FLASH_STATUS_OK:
                    log(f"远程烧录: 传输失败 @0x{cur_addr:08X}: {self._flash_resp_msg(resp)}")
                    return
                sent += len(chunk)
                offset += chunk_size
            log(f"远程烧录: 已传输 {sent}/{total_bytes} 字节")

        # Step 3: Send FINISH
        log("远程烧录: 数据传输完成, 开始编程...")
        await self._relay_send(bytes([WS_CHANNEL_FLASH, FLASH_CMD_FINISH]))

        # Wait for programming progress/result (may take a while)
        while True:
            resp = await self._wait_flash_resp(60)
            if not resp:
                log("远程烧录: 编程超时")
                return
            msg = self._flash_resp_msg(resp)
            if resp[0] == FLASH_STATUS_PROGRESS:
                log(f"远程烧录: {msg}")
            elif resp[0] == FLASH_STATUS_OK:
                log(f"远程烧录: 成功! {msg}")
                return
            elif resp[0] == FLASH_STATUS_ERROR:
                log(f"远程烧录: 失败: {msg}")
                return

    async def _wait_flash_resp(self, timeout: float):
        try:
            return await asyncio.wait_for(self._pending_flash.get(), timeout=timeout)
        except asyncio.TimeoutError:
            return None

    @staticmethod
    def _flash_resp_msg(resp):
        if resp and len(resp) > 1:
            return resp[1:].decode('utf-8', errors='replace')
        return "unknown"

    def _run_loop(self, relay_url, relay_host, relay_port, code, esp_ip, esp_port,
                  dap_port, serial_port, rtt_port, relay_protocol, relay_tcp_port):
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(
                self._main(relay_url, relay_host, relay_port, code, esp_ip, esp_port,
                           dap_port, serial_port, rtt_port, relay_protocol, relay_tcp_port))
        except Exception as e:
            self.log(f"桥接错误: {e}")
        finally:
            self._running = False
            self._paired = False

    async def _main(self, relay_url, relay_host, relay_port, code, esp_ip, esp_port,
                    dap_port, serial_port, rtt_port, relay_protocol, relay_tcp_port):
        if self._mode == "relay":
            if relay_protocol == "tcp":
                await self._run_tcp_relay(relay_host, relay_tcp_port, code,
                                          dap_port, serial_port, rtt_port)
            else:
                await self._run_relay(relay_url, relay_host, relay_port, code,
                                      dap_port, serial_port, rtt_port)
        else:
            await self._run_direct(esp_ip, esp_port, dap_port)

    # ── Relay mode ──

    async def _run_relay(self, relay_url, relay_host, relay_port, code,
                         dap_port, serial_port, rtt_port):
        if not websockets:
            self.log("错误: 未安装 websockets 库")
            return

        code = code.upper().strip()
        relay_url = relay_url.strip()
        relay_host = relay_host.strip()

        if relay_host:
            relay_url = f"ws://{relay_host}:{relay_port}"
        elif relay_url:
            if not relay_url.startswith("ws://") and not relay_url.startswith("wss://"):
                relay_url = "ws://" + relay_url
        else:
            self.log("错误: 请输入中继服务器地址")
            return

        self.log(f"连接中继: {relay_url}")
        try:
            self._ws = await websockets.connect(relay_url, max_size=2**20, open_timeout=30)
        except Exception as e:
            self.log(f"连接失败: {e}")
            return

        await self._ws.send(json.dumps({
            "type": "register", "role": "client", "code": code
        }))
        self.log(f"已注册, 配对码: {code}, 等待设备...")

        # Wait for pairing
        while self._running:
            try:
                msg = await asyncio.wait_for(self._ws.recv(), timeout=1.0)
                if isinstance(msg, str):
                    data = json.loads(msg)
                    if data.get("type") == "paired":
                        self._paired = True
                        self.log("✓ 设备已配对")
                        break
                    elif data.get("type") == "device_disconnected":
                        self.log("✗ 设备断开")
                        return
            except asyncio.TimeoutError:
                continue
            except websockets.exceptions.ConnectionClosed:
                self.log("✗ 中继断开")
                return

        if not self._paired:
            return

        # Start relay reader
        relay_task = asyncio.create_task(self._relay_reader())

        # Start TCP servers
        dap_server = await asyncio.start_server(
            self._handle_el_client, "127.0.0.1", dap_port, reuse_address=True)
        serial_server = await asyncio.start_server(
            self._handle_serial_client, "127.0.0.1", serial_port, reuse_address=True)
        rtt_server = await asyncio.start_server(
            self._handle_rtt_client, "127.0.0.1", rtt_port, reuse_address=True)

        self.log(f"=== 桥接就绪 ===")
        self.log(f"DAP:  127.0.0.1:{dap_port}")
        self.log(f"串口: 127.0.0.1:{serial_port}")
        self.log(f"RTT:  127.0.0.1:{rtt_port}")

        try:
            await relay_task
        except asyncio.CancelledError:
            pass
        finally:
            dap_server.close()
            serial_server.close()
            rtt_server.close()
            if self._ws:
                await self._ws.close()
            self._paired = False
            self.log("桥接已停止")

    async def _relay_reader(self):
        try:
            async for msg in self._ws:
                if isinstance(msg, bytes) and self._paired:
                    if len(msg) > 1:
                        ch = msg[0]
                        payload = msg[1:]
                        if ch == WS_CHANNEL_DAP:
                            if self._pipeline_depth > 0:
                                self._pipeline_depth -= 1
                            else:
                                await self._pending_dap.put(payload)
                        elif ch == WS_CHANNEL_SERIAL:
                            self._serial_rx_q.put(payload)
                        elif ch == WS_CHANNEL_RTT:
                            self._rtt_rx_q.put(payload)
                        elif ch == WS_CHANNEL_FLASH:
                            await self._pending_flash.put(payload)
                    else:
                        if self._pipeline_depth > 0:
                            self._pipeline_depth -= 1
                        else:
                            await self._pending_dap.put(msg)
                elif isinstance(msg, str):
                    d = json.loads(msg)
                    if d.get("type") == "device_disconnected":
                        self.log("✗ 设备断开")
                        self._paired = False
                        return
        except websockets.exceptions.ConnectionClosed:
            self.log("✗ 中继断开")
            self._paired = False

    async def _handle_el_client(self, reader, writer):
        addr = writer.get_extra_info("peername")
        self.log(f"elaphureLink 已连接: {addr}")
        handshake_done = False
        self._dap_info_cache.clear()
        self._pipeline_depth = 0

        while not self._pending_dap.empty():
            self._pending_dap.get_nowait()

        try:
            while self._running and self._paired:
                try:
                    data = await asyncio.wait_for(reader.read(1500), timeout=5.0)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break

                if not handshake_done:
                    endian = parse_el_handshake(data)
                    if endian:
                        writer.write(build_el_handshake_response(endian))
                        await writer.drain()
                        handshake_done = True
                        self.log("  握手完成")
                        continue
                    break

                cmd = data[0] if data else 0xFF

                # 1) DAP_Info (0x00): cache after first query
                if cmd == 0x00 and len(data) >= 2:
                    info_id = data[1]
                    if info_id in self._dap_info_cache:
                        writer.write(self._dap_info_cache[info_id])
                        await writer.drain()
                        continue

                # 2) DAP_HostStatus (0x01): safe to predict (no side-effect)
                if cmd == 0x01:
                    writer.write(bytes([0x01, 0x00]))
                    await writer.drain()
                    continue

                # 3) All other commands: forward to ESP32 and wait for response.
                #    No pipelining over WiFi relay — ESP32 uses a single-entry
                #    command buffer so rapid fire-and-forget causes overwrite.
                resp = await self._forward_dap_cmd(data)
                if resp is None:
                    break

                # Cache DAP_Info response
                if cmd == 0x00 and len(data) >= 2:
                    self._dap_info_cache[data[1]] = resp

                writer.write(resp)
                await writer.drain()
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            self.log(f"  错误: {e}")
        finally:
            self.log(f"elaphureLink 断开: {addr}")
            writer.close()

    @staticmethod
    def _predict_dap_response(cmd, data):
        """Return predicted response for safe-to-pipeline commands, or None.
        Only commands that ALWAYS succeed with a fixed response are listed."""
        if cmd == 0x01:                      # DAP_HostStatus
            return bytes([0x01, 0x00])
        if cmd == 0x04 and len(data) >= 6:   # DAP_TransferConfigure
            return bytes([0x04, 0x00])
        if cmd == 0x09 and len(data) >= 3:   # DAP_Delay
            return bytes([0x09, 0x00])
        if cmd == 0x11 and len(data) >= 5:   # DAP_SWJ_Clock
            return bytes([0x11, 0x00])
        if cmd == 0x12 and len(data) >= 2:   # DAP_SWJ_Sequence
            return bytes([0x12, 0x00])
        if cmd == 0x13 and len(data) >= 2:   # DAP_SWD_Configure
            return bytes([0x13, 0x00])
        return None

    def _send_dap_to_esp32(self, data):
        """Send DAP command to ESP32 fire-and-forget (response discarded via _pipeline_depth)."""
        if self._punch_dap_active:
            self._send_dap_via_udp(data)
        else:
            # Schedule async relay send
            asyncio.ensure_future(self._relay_send(bytes([WS_CHANNEL_DAP]) + data))

    async def _forward_dap_cmd(self, data):
        """Send DAP command to ESP32 and wait for response. Returns bytes or None."""
        if self._punch_dap_active:
            sent = self._send_dap_via_udp(data)
            if not sent:
                self.log("  ⚠ UDP发送失败, 回退中继")
                await self._relay_send(bytes([WS_CHANNEL_DAP]) + data)
        else:
            await self._relay_send(bytes([WS_CHANNEL_DAP]) + data)
        try:
            timeout = 2.0 if self._punch_dap_active else 30.0
            return await asyncio.wait_for(self._pending_dap.get(), timeout=timeout)
        except asyncio.TimeoutError:
            if self._punch_dap_active:
                self.log("  ⚠ UDP响应超时, 禁用直连")
                self._punch_dap_active = False
                await self._relay_send(bytes([WS_CHANNEL_DAP]) + data)
                try:
                    return await asyncio.wait_for(
                        self._pending_dap.get(), timeout=30.0)
                except asyncio.TimeoutError:
                    self.log("  ⚠ 响应超时 (30s)")
                    return None
            else:
                self.log("  ⚠ 响应超时 (30s)")
                return None

    async def _handle_serial_client(self, reader, writer):
        addr = writer.get_extra_info("peername")
        self.log(f"串口 TCP 已连接: {addr}")

        async def relay_to_tcp():
            try:
                while self._running and self._paired:
                    try:
                        data = await asyncio.wait_for(
                            asyncio.get_event_loop().run_in_executor(
                                None, lambda: self._serial_rx_q.get(timeout=0.5)), timeout=1.0)
                        writer.write(data)
                        await writer.drain()
                    except (asyncio.TimeoutError, queue.Empty):
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        fwd_task = asyncio.create_task(relay_to_tcp())
        try:
            while self._running and self._paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                await self._relay_send(bytes([WS_CHANNEL_SERIAL]) + data)
        except (asyncio.TimeoutError, ConnectionResetError, BrokenPipeError):
            pass
        finally:
            fwd_task.cancel()
            self.log(f"串口 TCP 断开: {addr}")
            writer.close()

    async def _handle_rtt_client(self, reader, writer):
        addr = writer.get_extra_info("peername")
        self.log(f"RTT TCP 已连接: {addr}")

        async def relay_to_tcp():
            try:
                while self._running and self._paired:
                    try:
                        data = await asyncio.wait_for(
                            asyncio.get_event_loop().run_in_executor(
                                None, lambda: self._rtt_rx_q.get(timeout=0.5)), timeout=1.0)
                        writer.write(data)
                        await writer.drain()
                    except (asyncio.TimeoutError, queue.Empty):
                        continue
            except (ConnectionResetError, BrokenPipeError):
                pass

        fwd_task = asyncio.create_task(relay_to_tcp())
        try:
            while self._running and self._paired:
                try:
                    data = await asyncio.wait_for(reader.read(256), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break
                await self._relay_send(bytes([WS_CHANNEL_RTT]) + data)
        except (asyncio.TimeoutError, ConnectionResetError, BrokenPipeError):
            pass
        finally:
            fwd_task.cancel()
            self.log(f"RTT TCP 断开: {addr}")
            writer.close()

    # ── TCP relay mode ──

    async def _run_tcp_relay(self, relay_host, relay_tcp_port, code,
                             dap_port, serial_port, rtt_port):
        code = code.upper().strip()
        relay_host = relay_host.strip()
        self._relay_host = relay_host  # Store for STUN probe

        if not relay_host:
            self.log("错误: 请输入中继服务器地址")
            return

        self.log(f"TCP中继连接: {relay_host}:{relay_tcp_port}")
        try:
            self._tcp_reader, self._tcp_writer = await asyncio.open_connection(
                relay_host, relay_tcp_port)
        except Exception as e:
            self.log(f"TCP连接失败: {e}")
            return

        # Send registration: [role=0x02(client), code_len, code...]
        code_bytes = code.encode('ascii')
        reg_msg = bytes([0x02, len(code_bytes)]) + code_bytes
        self._tcp_writer.write(reg_msg)
        await self._tcp_writer.drain()
        self.log(f"已注册, 配对码: {code}, 等待设备...")

        # Wait for pairing response
        while self._running:
            try:
                # Read exactly 1 byte for status
                status_byte = await asyncio.wait_for(
                    self._read_exact(1), timeout=1.0)
                if not status_byte:
                    self.log("✗ TCP中继断开")
                    return
                status = status_byte[0]
                if status == 0x00:
                    # Registered, waiting for peer
                    continue
                elif status == 0x01:
                    # Paired: [0x01, port_hi, port_lo, ip_len, ip...]
                    # Read port (2 bytes) + ip_len (1 byte)
                    rest = await asyncio.wait_for(
                        self._read_exact(3), timeout=5.0)
                    if not rest:
                        self.log("✓ 设备已配对")
                        self._paired = True
                        break
                    peer_port = (rest[0] << 8) | rest[1]
                    ip_len = rest[2]
                    peer_ip = None
                    if ip_len > 0:
                        ip_bytes = await asyncio.wait_for(
                            self._read_exact(ip_len), timeout=5.0)
                        if ip_bytes:
                            peer_ip = ip_bytes.decode('ascii', errors='replace')
                    if peer_ip:
                        self.log(f"✓ 设备已配对 (peer: {peer_ip}:{peer_port})")
                    else:
                        self.log("✓ 设备已配对")
                    self._paired = True
                    self._punch_peer_ip = peer_ip
                    self._punch_peer_port = peer_port
                    break
                elif status == 0x02:
                    self.log("✗ 设备已断开")
                    return
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                self.log(f"✗ TCP中继错误: {e}")
                return

        if not self._paired:
            return

        # Attempt NAT hole punching (background, non-blocking)
        if self._punch_peer_ip:
            await self._punch_init(self._punch_peer_ip, self._punch_peer_port)

        # Start TCP relay reader
        relay_task = asyncio.create_task(self._tcp_relay_reader())

        # Start TCP servers for local clients
        dap_server = await asyncio.start_server(
            self._handle_el_client, "127.0.0.1", dap_port, reuse_address=True)
        serial_server = await asyncio.start_server(
            self._handle_serial_client, "127.0.0.1", serial_port, reuse_address=True)
        rtt_server = await asyncio.start_server(
            self._handle_rtt_client, "127.0.0.1", rtt_port, reuse_address=True)

        self.log(f"=== TCP中继桥接就绪 ===")
        self.log(f"DAP:  127.0.0.1:{dap_port}")
        self.log(f"串口: 127.0.0.1:{serial_port}")
        self.log(f"RTT:  127.0.0.1:{rtt_port}")

        try:
            await relay_task
        except asyncio.CancelledError:
            pass
        finally:
            dap_server.close()
            serial_server.close()
            rtt_server.close()
            if self._punch_udp_transport:
                self._punch_udp_transport.close()
                self._punch_udp_transport = None
            self._punch_success = False
            self._punch_dap_active = False
            if self._tcp_writer:
                self._tcp_writer.close()
                self._tcp_writer = None
                self._tcp_reader = None
            self._paired = False
            self.log("TCP中继桥接已停止")

    async def _tcp_relay_reader(self):
        """Read length-prefixed messages from TCP relay and dispatch."""
        try:
            while self._running and self._paired:
                # Read 2-byte length header
                try:
                    hdr = await asyncio.wait_for(
                        self._read_exact(2), timeout=5.0)
                except asyncio.TimeoutError:
                    continue  # No data yet, keep waiting
                if hdr is None:
                    self.log("✗ TCP中继断开")
                    self._paired = False
                    return
                msg_len = (hdr[0] << 8) | hdr[1]
                if msg_len <= 0 or msg_len > 4096:
                    continue

                # Read message body
                try:
                    body = await asyncio.wait_for(
                        self._read_exact(msg_len), timeout=10.0)
                except asyncio.TimeoutError:
                    self.log("⚠ TCP中继: 消息体读取超时")
                    continue
                if body is None:
                    self._paired = False
                    return

                # Check for length-prefixed disconnect [0x00, 0x01, 0x02]
                if msg_len == 1 and body[0] == 0x02:
                    self.log("✗ 设备断开")
                    self._paired = False
                    return

                # Dispatch by channel
                if len(body) > 1:
                    ch = body[0]
                    payload = body[1:]
                    if ch == WS_CHANNEL_DAP:
                        if self._pipeline_depth > 0:
                            self._pipeline_depth -= 1
                        else:
                            await self._pending_dap.put(payload)
                    elif ch == WS_CHANNEL_SERIAL:
                        self._serial_rx_q.put(payload)
                    elif ch == WS_CHANNEL_RTT:
                        self._rtt_rx_q.put(payload)
                    elif ch == WS_CHANNEL_FLASH:
                        await self._pending_flash.put(payload)
                    elif ch == WS_CHANNEL_PUNCH:
                        self._handle_punch_msg(payload)
                elif len(body) == 1:
                    if self._pipeline_depth > 0:
                        self._pipeline_depth -= 1
                    else:
                        await self._pending_dap.put(body)
        except (ConnectionResetError, BrokenPipeError, OSError) as e:
            self.log(f"✗ TCP中继断开: {e}")
            self._paired = False

    async def _read_exact(self, n):
        """Read exactly n bytes from TCP relay, return None on disconnect."""
        data = b''
        while len(data) < n:
            chunk = await self._tcp_reader.read(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    # ── NAT Hole Punch ──

    def _handle_punch_msg(self, payload):
        """Handle punch control message from ESP32 via relay."""
        if len(payload) < 1:
            return
        if payload[0] == PUNCH_MY_PORT and len(payload) >= 3:
            self._punch_peer_udp_port = (payload[1] << 8) | payload[2]
            self.log(f"NAT穿透: 设备外部 UDP 端口 = {self._punch_peer_udp_port}")
            # Start punch if we already have our own external port from STUN
            if (self._loop and self._punch_peer_ip and
                    self._punch_peer_udp_port and
                    getattr(self, '_punch_my_ext_port', 0) > 0):
                asyncio.ensure_future(self._punch_start_sending(), loop=self._loop)
        elif payload[0] == PUNCH_DIRECT:
            self.log("NAT穿透: 设备已切换到直连模式")
            self._punch_dap_active = True

    async def _punch_init(self, peer_ip, peer_port):
        """Initialize UDP hole punch after TCP pairing. Uses STUN to discover external port."""
        self._punch_peer_ip = peer_ip
        self._punch_peer_port = peer_port
        self._punch_success = False
        self._punch_dap_active = False

        relay_host = getattr(self, '_relay_host', '')
        if not relay_host:
            self.log("NAT穿透: 无中继地址，跳过")
            return

        loop = asyncio.get_event_loop()

        class PunchProtocol(asyncio.DatagramProtocol):
            def __init__(self, bridge):
                self.bridge = bridge
                self.stun_event = asyncio.Event()
                self.ext_port = 0

            def datagram_received(self, data, addr):
                b = self.bridge
                # STUN response: [port_hi, port_lo, ip_ascii...]
                if not self.stun_event.is_set() and len(data) >= 3:
                    self.ext_port = (data[0] << 8) | data[1]
                    ext_ip = data[2:].decode('ascii', errors='replace')
                    b.log(f"NAT穿透: STUN 外部地址 {ext_ip}:{self.ext_port}")
                    b._punch_my_ext_port = self.ext_port
                    self.stun_event.set()
                    return

                if data == PUNCH_MAGIC and not b._punch_success:
                    b._punch_success = True
                    b._punch_direct_addr = addr
                    b.log(f"NAT穿透: 成功! 直连地址: {addr[0]}:{addr[1]}")
                    if b._punch_udp_transport:
                        b._punch_udp_transport.sendto(PUNCH_ACK, addr)
                    asyncio.ensure_future(
                        b._relay_send(bytes([WS_CHANNEL_PUNCH, PUNCH_DIRECT])))
                    b._punch_dap_active = True
                elif data == PUNCH_ACK and not b._punch_success:
                    b._punch_success = True
                    b._punch_direct_addr = addr
                    b.log(f"NAT穿透: ACK收到! 直连地址: {addr[0]}:{addr[1]}")
                    b._punch_dap_active = True
                elif b._punch_dap_active and len(data) >= 2:
                    ch = data[0]
                    payload = data[1:]
                    if ch == WS_CHANNEL_DAP:
                        try:
                            if b._pipeline_depth > 0:
                                b._pipeline_depth -= 1
                            else:
                                b._pending_dap.put_nowait(payload)
                        except asyncio.QueueFull:
                            pass

            def error_received(self, exc):
                pass

        try:
            transport, protocol = await loop.create_datagram_endpoint(
                lambda: PunchProtocol(self),
                local_addr=('0.0.0.0', 0))
            self._punch_udp_transport = transport
            self._punch_udp_protocol = protocol

            # Send STUN probe and wait for response
            stun_addr = (relay_host, STUN_PORT)
            for attempt in range(5):
                transport.sendto(b"STUN", stun_addr)
                if attempt == 0:
                    self.log(f"NAT穿透: STUN 探测 → {relay_host}:{STUN_PORT}")
                try:
                    await asyncio.wait_for(protocol.stun_event.wait(), timeout=1.0)
                    break
                except asyncio.TimeoutError:
                    pass

            if not protocol.stun_event.is_set():
                self.log("NAT穿透: STUN 无响应")
                return

            # Send our EXTERNAL UDP port to peer via relay (directly, not via callback)
            ext_port = protocol.ext_port
            await self._relay_send(bytes([
                WS_CHANNEL_PUNCH, PUNCH_MY_PORT,
                (ext_port >> 8) & 0xFF, ext_port & 0xFF]))
            self.log(f"NAT穿透: 已发送外部端口 {ext_port} 给设备")

            # If peer's port is already known, start punching in background
            if self._punch_peer_udp_port:
                asyncio.ensure_future(self._punch_start_sending())

        except Exception as e:
            self.log(f"NAT穿透: 初始化失败: {e}")

    async def _punch_start_sending(self):
        """Send punch packets to peer's UDP port."""
        if not self._punch_udp_transport or not self._punch_peer_ip:
            return

        target_addr = (self._punch_peer_ip, self._punch_peer_udp_port)
        self.log(f"NAT穿透: 开始打洞 → {target_addr[0]}:{target_addr[1]}")

        for i in range(50):  # 50 * 200ms = 10s
            if not self._running or self._punch_success:
                break
            try:
                self._punch_udp_transport.sendto(PUNCH_MAGIC, target_addr)
            except Exception:
                pass
            await asyncio.sleep(0.2)

        if self._punch_success:
            self.log("NAT穿透: 直连 DAP 通道已激活")
            asyncio.ensure_future(self._punch_keepalive())
        else:
            self.log("NAT穿透: 超时, 继续使用中继")

    async def _punch_keepalive(self):
        """Send periodic UDP keepalive to prevent NAT mapping expiry."""
        while (self._running and self._paired and
               self._punch_dap_active and self._punch_udp_transport and
               self._punch_direct_addr):
            try:
                self._punch_udp_transport.sendto(PUNCH_ACK, self._punch_direct_addr)
            except Exception:
                break
            await asyncio.sleep(15)

    def _send_dap_via_udp(self, data: bytes):
        """Send DAP command directly via UDP (non-async, used from handle_el_client)."""
        if self._punch_udp_transport and self._punch_direct_addr:
            try:
                msg = bytes([WS_CHANNEL_DAP]) + data
                self._punch_udp_transport.sendto(msg, self._punch_direct_addr)
                return True
            except Exception:
                return False
        return False

    # ── Direct mode ──

    async def _run_direct(self, esp_ip, esp_port, dap_port):
        """Direct TCP proxy: connect to ESP32 TCP, expose local elaphureLink server."""
        esp_port = int(esp_port)
        if ":" in esp_ip:
            parts = esp_ip.rsplit(":", 1)
            esp_ip = parts[0]
            esp_port = int(parts[1])

        self.log(f"直连 ESP32: {esp_ip}:{esp_port}")
        try:
            self._direct_reader, self._direct_writer = await asyncio.open_connection(
                esp_ip, esp_port)
        except Exception as e:
            self.log(f"连接失败: {e}")
            return

        # elaphureLink handshake with ESP32
        handshake = struct.pack(">III", EL_IDENTIFIER, EL_CMD_HANDSHAKE, EL_DAP_VERSION)
        self._direct_writer.write(handshake)
        await self._direct_writer.drain()
        resp = await asyncio.wait_for(self._direct_reader.read(12), timeout=5.0)
        if len(resp) != 12:
            self.log("ESP32 握手失败")
            return
        self.log("✓ ESP32 已连接")
        self._paired = True

        # Local TCP server for elaphureLink (Keil)
        dap_server = await asyncio.start_server(
            self._handle_direct_el_client, "127.0.0.1", dap_port, reuse_address=True)
        self.log(f"DAP: 127.0.0.1:{dap_port}")

        try:
            while self._running:
                await asyncio.sleep(0.5)
        except asyncio.CancelledError:
            pass
        finally:
            dap_server.close()
            if self._direct_writer:
                self._direct_writer.close()
            self._paired = False
            self.log("直连已停止")

    async def _handle_direct_el_client(self, reader, writer):
        """Proxy elaphureLink client ↔ ESP32 direct TCP."""
        addr = writer.get_extra_info("peername")
        self.log(f"elaphureLink 已连接: {addr}")
        handshake_done = False

        try:
            while self._running and self._paired:
                try:
                    data = await asyncio.wait_for(reader.read(1500), timeout=5.0)
                except asyncio.TimeoutError:
                    continue
                if not data:
                    break

                if not handshake_done:
                    endian = parse_el_handshake(data)
                    if endian:
                        writer.write(build_el_handshake_response(endian))
                        await writer.drain()
                        handshake_done = True
                        self.log("  握手完成")
                        continue
                    break

                # Forward to ESP32
                self._direct_writer.write(data)
                await self._direct_writer.drain()
                resp = await asyncio.wait_for(self._direct_reader.read(4096), timeout=5.0)
                if resp:
                    writer.write(resp)
                    await writer.drain()
                else:
                    break
        except (ConnectionResetError, BrokenPipeError):
            pass
        except Exception as e:
            self.log(f"  错误: {e}")
        finally:
            self.log(f"elaphureLink 断开: {addr}")
            writer.close()


# ─── Serial Engine ────────────────────────────────────────────────────────────

class SerialEngine:
    def __init__(self, rx_queue: queue.Queue, log_queue: queue.Queue):
        self._rx_q = rx_queue
        self._log_q = log_queue
        self._ser = None   # serial.Serial or None
        self._thread = None  # threading.Thread or None
        self._running = False

    @property
    def is_open(self):
        return self._ser is not None and self._ser.is_open

    def open(self, port: str, baud: int):
        if not serial:
            self._log_q.put("错误: 未安装 pyserial")
            return False
        try:
            self._ser = serial.Serial(port, baud, timeout=0.1)
            self._ser.dtr = True   # Trigger ESP32 to print IP/Code
            self._running = True
            self._thread = threading.Thread(target=self._read_loop, daemon=True)
            self._thread.start()
            self._log_q.put(f"串口已打开: {port} @ {baud}")
            return True
        except Exception as e:
            self._log_q.put(f"串口打开失败: {e}")
            return False

    def close(self):
        self._running = False
        if self._ser and self._ser.is_open:
            self._ser.close()
        self._ser = None
        self._log_q.put("串口已关闭")

    def write(self, data: bytes):
        if self._ser and self._ser.is_open:
            self._ser.write(data)

    def _read_loop(self):
        while self._running and self._ser and self._ser.is_open:
            try:
                data = self._ser.read(256)
                if data:
                    self._rx_q.put(data)
            except Exception:
                break

    @staticmethod
    def list_ports():
        if not serial:
            return []
        return [p.device for p in serial.tools.list_ports.comports()]


class TcpClientEngine:
    """TCP client serial terminal — connects to a remote IP:PORT."""

    def __init__(self, rx_queue: queue.Queue, log_queue: queue.Queue):
        self._rx_q = rx_queue
        self._log_q = log_queue
        self._sock = None
        self._thread = None
        self._running = False

    @property
    def is_open(self):
        return self._sock is not None and self._running

    def open(self, host: str, port: int):
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock.settimeout(5)
            self._sock.connect((host, port))
            self._sock.settimeout(0.2)
            self._running = True
            self._thread = threading.Thread(target=self._read_loop, daemon=True)
            self._thread.start()
            self._log_q.put(f"TCP 已连接: {host}:{port}")
            return True
        except Exception as e:
            self._log_q.put(f"TCP 连接失败: {e}")
            if self._sock:
                try:
                    self._sock.close()
                except Exception:
                    pass
                self._sock = None
            return False

    def close(self):
        self._running = False
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None
        self._log_q.put("TCP 已断开")

    def write(self, data: bytes):
        if self._sock and self._running:
            try:
                self._sock.sendall(data)
            except Exception:
                pass

    def _read_loop(self):
        while self._running and self._sock:
            try:
                data = self._sock.recv(4096)
                if data:
                    self._rx_q.put(data)
                else:
                    self._log_q.put("TCP 远程断开")
                    break
            except socket.timeout:
                continue
            except Exception:
                break
        self._running = False


# ─── Chart Window ─────────────────────────────────────────────────────────────

class _ChartWindow(tk.Toplevel):
    """Real-time chart window for plotting parsed protocol data."""

    COLORS = ["#FF4444", "#4488FF", "#44CC44", "#FF8800", "#AA44FF",
              "#00CCCC", "#FF44AA", "#88AA00"]

    def __init__(self, master, checked_iids, history, rx_tree):
        super().__init__(master)
        self.title("数据曲线")
        self.geometry("800x500")
        self.transient(master)
        self._master_app = master
        self._checked_iids = list(checked_iids)
        self._history = history
        self._rx_tree = rx_tree
        self._paused = False
        self._saving_csv = False
        self._csv_file = None
        self._csv_writer = None
        self._field_names = []

        # Collect field names
        for iid in self._checked_iids:
            try:
                field_name = self._rx_tree.set(iid, "field")
                proto_name = self._rx_tree.set(iid, "protocol")
                label_text = f"{proto_name}_{field_name}" if proto_name else field_name
            except Exception:
                label_text = iid
            self._field_names.append(label_text)

        # Top bar
        bar = ttk.Frame(self)
        bar.pack(fill=tk.X, padx=4, pady=4)
        self._pause_btn = ttk.Button(bar, text="暂停", command=self._toggle_pause)
        self._pause_btn.pack(side=tk.LEFT, padx=4)
        ttk.Button(bar, text="清空历史", command=self._clear_history).pack(side=tk.LEFT, padx=4)

        ttk.Label(bar, text="显示点数:").pack(side=tk.LEFT, padx=(12, 2))
        self._points_var = tk.StringVar(value="200")
        ttk.Entry(bar, textvariable=self._points_var, width=6).pack(side=tk.LEFT, padx=2)

        ttk.Label(bar, text="刷新(ms):").pack(side=tk.LEFT, padx=(12, 2))
        self._refresh_var = tk.StringVar(value="50")
        ttk.Entry(bar, textvariable=self._refresh_var, width=5).pack(side=tk.LEFT, padx=2)

        self._csv_btn = ttk.Button(bar, text="保存CSV", command=self._toggle_csv)
        self._csv_btn.pack(side=tk.RIGHT, padx=4)
        ttk.Button(bar, text="重置缩放", command=self._reset_zoom).pack(side=tk.RIGHT, padx=4)

        # Legend
        legend_frame = ttk.Frame(self)
        legend_frame.pack(fill=tk.X, padx=4, pady=2)
        for ci, label_text in enumerate(self._field_names):
            color = self.COLORS[ci % len(self.COLORS)]
            tk.Label(legend_frame, text="■ " + label_text, fg=color,
                     font=("", 9)).pack(side=tk.LEFT, padx=6)

        # Canvas
        self._canvas = tk.Canvas(self, bg="white", highlightthickness=0)
        self._canvas.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        # Zoom state: 1.0 = default, >1 = zoom in, <1 = zoom out
        self._x_zoom = 1.0  # X axis zoom factor (affects displayed points)
        self._y_zoom = 1.0  # Y axis zoom factor
        self._y_offset = 0.0  # Y axis pan offset (fraction of range)

        # Bind mouse wheel for zoom
        self._canvas.bind("<MouseWheel>", self._on_mousewheel)
        # Margins for axis detection
        self._margin_l = 60
        self._margin_r = 20
        self._margin_t = 20
        self._margin_b = 30

        self._last_csv_count = {iid: 0 for iid in self._checked_iids}
        self._update_job = None
        self._schedule_update()

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _on_mousewheel(self, event):
        """Handle mouse wheel: zoom X or Y axis depending on cursor position."""
        # Determine zoom direction
        delta = 1 if event.delta > 0 else -1
        factor = 1.2 if delta > 0 else 1 / 1.2

        w = self._canvas.winfo_width()
        h = self._canvas.winfo_height()
        mx, my = event.x, event.y

        plot_left = self._margin_l
        plot_right = w - self._margin_r
        plot_top = self._margin_t
        plot_bottom = h - self._margin_b

        # If mouse is in the Y-axis label area (left of plot) or left portion of plot
        if mx < plot_left:
            # Zoom Y axis
            self._y_zoom = max(0.1, min(self._y_zoom * factor, 100.0))
        elif my > plot_bottom:
            # Mouse in X-axis label area (below plot): zoom X axis
            self._x_zoom = max(0.1, min(self._x_zoom * factor, 100.0))
        elif mx >= plot_left and mx <= plot_right and my >= plot_top and my <= plot_bottom:
            # Inside plot area: zoom X axis by default
            self._x_zoom = max(0.1, min(self._x_zoom * factor, 100.0))

        if self._paused:
            self._draw()

    def _reset_zoom(self):
        """Reset zoom to default."""
        self._x_zoom = 1.0
        self._y_zoom = 1.0
        if self._paused:
            self._draw()

    def _toggle_pause(self):
        self._paused = not self._paused
        self._pause_btn.config(text="继续" if self._paused else "暂停")

    def _clear_history(self):
        for iid in self._checked_iids:
            if iid in self._history:
                self._history[iid].clear()
        self._draw()

    def _toggle_csv(self):
        if self._saving_csv:
            self._stop_csv()
        else:
            self._start_csv()

    def _start_csv(self):
        import csv as csv_mod
        path = filedialog.asksaveasfilename(
            title="保存CSV文件",
            filetypes=[("CSV", "*.csv"), ("All Files", "*.*")],
            defaultextension=".csv",
        )
        if not path:
            return
        try:
            self._csv_file = open(path, "w", newline="", encoding="utf-8")
            self._csv_writer = csv_mod.writer(self._csv_file)
            header = ["timestamp"] + self._field_names
            self._csv_writer.writerow(header)
            self._last_csv_count = {iid: len(self._history.get(iid, [])) for iid in self._checked_iids}
            self._saving_csv = True
            self._csv_btn.config(text="停止保存")
        except Exception as e:
            if self._csv_file:
                self._csv_file.close()
                self._csv_file = None
            self._master_app._log_q.put(f"CSV保存失败: {e}")

    def _stop_csv(self):
        self._saving_csv = False
        self._csv_btn.config(text="保存CSV")
        if self._csv_file:
            self._csv_file.close()
            self._csv_file = None
            self._csv_writer = None

    def _write_csv_new_data(self):
        if not self._saving_csv or not self._csv_writer:
            return
        # Find new data points and write them
        max_new = 0
        new_data = {}
        for iid in self._checked_iids:
            data = self._history.get(iid, [])
            prev_count = self._last_csv_count.get(iid, 0)
            new_points = data[prev_count:]
            new_data[iid] = new_points
            self._last_csv_count[iid] = len(data)
            if len(new_points) > max_new:
                max_new = len(new_points)
        if max_new == 0:
            return
        # Write rows - align by index
        for ri in range(max_new):
            row = [""]
            ts = ""
            for iid in self._checked_iids:
                points = new_data.get(iid, [])
                if ri < len(points):
                    if not ts:
                        ts = time.strftime("%Y-%m-%d %H:%M:%S.", time.localtime(points[ri][0])) + \
                             f"{int(points[ri][0]*1000)%1000:03d}"
                    row.append(str(points[ri][1]))
                else:
                    row.append("")
            row[0] = ts
            self._csv_writer.writerow(row)
        try:
            self._csv_file.flush()
        except Exception:
            pass

    def _schedule_update(self):
        if not self._paused:
            self._draw()
        self._write_csv_new_data()
        try:
            interval = int(self._refresh_var.get())
            interval = max(16, min(interval, 2000))
        except ValueError:
            interval = 50
        self._update_job = self.after(interval, self._schedule_update)

    def _on_close(self):
        if self._update_job is not None:
            self.after_cancel(self._update_job)
        self._stop_csv()
        self.destroy()

    def _draw(self):
        c = self._canvas
        c.delete("all")
        w = c.winfo_width()
        h = c.winfo_height()
        if w < 50 or h < 50:
            return

        try:
            max_points = int(self._points_var.get())
        except ValueError:
            max_points = 200
        max_points = max(10, min(max_points, 10000))

        # Apply X zoom: fewer points = more detail per point
        effective_points = max(10, int(max_points / self._x_zoom))

        margin_l = self._margin_l
        margin_r = self._margin_r
        margin_t = self._margin_t
        margin_b = self._margin_b
        plot_w = w - margin_l - margin_r
        plot_h = h - margin_t - margin_b

        if plot_w < 10 or plot_h < 10:
            return

        # Gather data using effective_points
        all_values = []
        series_data = []
        for iid in self._checked_iids:
            data = self._history.get(iid, [])
            if len(data) > effective_points:
                data = data[-effective_points:]
            series_data.append(data)
            all_values.extend(v for _, v in data)

        if not all_values:
            c.create_text(w // 2, h // 2, text="等待数据...", font=("", 12), fill="gray")
            return

        # Compute Y range
        y_min_raw = min(all_values)
        y_max_raw = max(all_values)
        if y_min_raw == y_max_raw:
            y_min_raw -= 1
            y_max_raw += 1
        y_range_raw = y_max_raw - y_min_raw
        y_padding = y_range_raw * 0.05
        y_min_raw -= y_padding
        y_max_raw += y_padding

        # Apply Y zoom: shrink range around center
        y_center = (y_min_raw + y_max_raw) / 2
        y_half = (y_max_raw - y_min_raw) / 2 / self._y_zoom
        y_min = y_center - y_half
        y_max = y_center + y_half
        y_range = y_max - y_min

        # Get timestamps for X-axis labels
        longest_series = max(series_data, key=len) if series_data else []
        first_ts = longest_series[0][0] if longest_series else time.time()
        last_ts = longest_series[-1][0] if longest_series else time.time()

        # Update title
        total_pts = sum(len(self._history.get(iid, [])) for iid in self._checked_iids)
        zoom_info = ""
        if self._x_zoom != 1.0 or self._y_zoom != 1.0:
            zoom_info = f" X:{self._x_zoom:.1f}x Y:{self._y_zoom:.1f}x"
        self.title(f"数据曲线 (显示:{effective_points} 总:{total_pts}{zoom_info})")

        # Draw grid
        c.create_rectangle(margin_l, margin_t, margin_l + plot_w, margin_t + plot_h,
                           outline="#CCCCCC")

        # Y axis labels and grid lines
        n_yticks = 5
        for i in range(n_yticks + 1):
            frac = i / n_yticks
            y_val = y_min + frac * y_range
            y_px = margin_t + plot_h - frac * plot_h
            c.create_line(margin_l, y_px, margin_l + plot_w, y_px, fill="#EEEEEE")
            label = f"{y_val:.2f}" if abs(y_val) < 10000 else f"{y_val:.1e}"
            c.create_text(margin_l - 4, y_px, text=label, anchor=tk.E, font=("", 8))

        # X axis labels
        n_xticks = 5
        ts_range = last_ts - first_ts
        if ts_range <= 0:
            ts_range = 1
        for i in range(n_xticks + 1):
            frac = i / n_xticks
            x_px = margin_l + frac * plot_w
            c.create_line(x_px, margin_t, x_px, margin_t + plot_h, fill="#EEEEEE")
            t_val = first_ts + frac * ts_range
            t_str = time.strftime("%H:%M:%S", time.localtime(t_val))
            c.create_text(x_px, margin_t + plot_h + 4, text=t_str, anchor=tk.N, font=("", 8))

        # Draw series - index-based X for uniform spacing
        for si, data in enumerate(series_data):
            if len(data) < 2:
                continue
            color = self.COLORS[si % len(self.COLORS)]
            n = len(data)
            points = []
            for idx, (t_val, y_val) in enumerate(data):
                x_px = margin_l + (idx / (n - 1)) * plot_w
                y_px = margin_t + plot_h - ((y_val - y_min) / y_range) * plot_h
                points.append(x_px)
                points.append(y_px)
            if len(points) >= 4:
                c.create_line(*points, fill=color, width=1.5)


# ─── GUI ──────────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("960x650")
        self.minsize(800, 550)

        self.cfg = AppConfig()

        # Queues for inter-thread communication
        self._log_q = queue.Queue()
        self._serial_rx_q = queue.Queue()     # serial COM data
        self._bridge_serial_q = queue.Queue() # serial via bridge
        self._rtt_rx_q = queue.Queue()

        # Engines
        self.serial_engine = SerialEngine(self._serial_rx_q, self._log_q)
        self.tcp_client_engine = TcpClientEngine(self._serial_rx_q, self._log_q)
        self.bridge_engine = BridgeEngine(self._log_q, self._bridge_serial_q, self._rtt_rx_q)
        self._el_proc = None    # subprocess.Popen or None
        self._el_proxy = None   # ElProxy instance or None

        self._build_ui()
        self._poll_queues()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self):
        # Notebook tabs
        self._nb = ttk.Notebook(self)
        self._nb.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self._build_terminal_tab()
        self._build_protocol_tab()
        self._build_bridge_tab()
        self._build_log_tab()

    # ── Terminal Tab (Serial + RTT) ──

    def _build_terminal_tab(self):
        f = ttk.Frame(self._nb)
        self._nb.add(f, text="  终端  ")

        split = ttk.PanedWindow(f, orient=tk.HORIZONTAL)
        split.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        main = ttk.Frame(split)
        side = ttk.Frame(split, width=240)
        split.add(main, weight=4)
        split.add(side, weight=1)

        # Top bar - connection type selector
        bar = ttk.Frame(main)
        bar.pack(fill=tk.X, padx=4, pady=4)

        self._term_mode_var = tk.StringVar(value=self.cfg.terminal_mode)
        ttk.Radiobutton(bar, text="串口", variable=self._term_mode_var,
                        value="serial", command=self._term_mode_changed).pack(side=tk.LEFT)
        ttk.Radiobutton(bar, text="TCP Client", variable=self._term_mode_var,
                        value="tcp_client", command=self._term_mode_changed).pack(side=tk.LEFT, padx=(4, 8))

        # Serial widgets
        self._serial_widgets = []
        w = ttk.Label(bar, text="端口:")
        w.pack(side=tk.LEFT)
        self._serial_widgets.append(w)
        self._com_var = tk.StringVar(value=self.cfg.serial_port)
        self._com_cb = ttk.Combobox(bar, textvariable=self._com_var, width=12)
        self._com_cb.pack(side=tk.LEFT, padx=2)
        self._serial_widgets.append(self._com_cb)
        w = ttk.Button(bar, text="⟳", width=3, command=self._refresh_ports)
        w.pack(side=tk.LEFT)
        self._serial_widgets.append(w)
        w = ttk.Label(bar, text="波特率:")
        w.pack(side=tk.LEFT, padx=(8, 0))
        self._serial_widgets.append(w)
        self._baud_var = tk.StringVar(value=str(self.cfg.serial_baud))
        w = ttk.Combobox(bar, textvariable=self._baud_var, values=BAUD_RATES, width=8)
        w.pack(side=tk.LEFT, padx=2)
        self._serial_widgets.append(w)

        # TCP client widgets
        self._tcp_widgets = []
        w = ttk.Label(bar, text="IP:")
        w.pack(side=tk.LEFT)
        self._tcp_widgets.append(w)
        self._tcp_host_var = tk.StringVar(value=self.cfg.tcp_client_host)
        w = ttk.Entry(bar, textvariable=self._tcp_host_var, width=16)
        w.pack(side=tk.LEFT, padx=2)
        self._tcp_widgets.append(w)
        w = ttk.Label(bar, text="Port:")
        w.pack(side=tk.LEFT, padx=(4, 0))
        self._tcp_widgets.append(w)
        self._tcp_port_var = tk.StringVar(value=str(self.cfg.tcp_client_port))
        w = ttk.Entry(bar, textvariable=self._tcp_port_var, width=6)
        w.pack(side=tk.LEFT, padx=2)
        self._tcp_widgets.append(w)

        self._serial_btn = ttk.Button(bar, text="打开", command=self._toggle_serial)
        self._serial_btn.pack(side=tk.LEFT, padx=8)

        ttk.Button(bar, text="清屏", command=lambda: self._serial_text.delete("1.0", tk.END)
                   ).pack(side=tk.RIGHT)

        self._serial_hex_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="HEX", variable=self._serial_hex_var).pack(side=tk.RIGHT, padx=4)

        self._serial_ts_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="时间戳", variable=self._serial_ts_var).pack(side=tk.RIGHT, padx=4)

        self._serial_show_send_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="显示发送", variable=self._serial_show_send_var).pack(side=tk.RIGHT, padx=4)

        self._serial_wrap_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="自动换行", variable=self._serial_wrap_var,
                        command=self._toggle_serial_wrap).pack(side=tk.RIGHT, padx=4)

        # Track whether next inserted text needs a line-start prefix
        self._ts_need_prefix = True

        # Text area
        self._serial_text = scrolledtext.ScrolledText(main, wrap=tk.WORD, font=("Consolas", 10))
        self._serial_text.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 2))

        # Send bar
        send_bar = ttk.Frame(main)
        send_bar.pack(fill=tk.X, padx=4, pady=(0, 4))
        self._serial_send_var = tk.StringVar()
        entry = ttk.Entry(send_bar, textvariable=self._serial_send_var)
        entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4))
        entry.bind("<Return>", lambda e: self._serial_send())

        self._serial_send_hex = tk.BooleanVar(value=False)
        ttk.Checkbutton(send_bar, text="HEX发送", variable=self._serial_send_hex).pack(side=tk.LEFT, padx=2)
        ttk.Button(send_bar, text="发送", command=self._serial_send).pack(side=tk.LEFT)

        ttk.Separator(send_bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=6)
        self._cycle_interval_var = tk.StringVar(value="1000")
        ttk.Label(send_bar, text="周期(ms):").pack(side=tk.LEFT)
        ttk.Entry(send_bar, textvariable=self._cycle_interval_var, width=6).pack(side=tk.LEFT, padx=2)
        self._cycle_btn = ttk.Button(send_bar, text="周期发送", command=self._toggle_cycle_send)
        self._cycle_btn.pack(side=tk.LEFT, padx=2)
        self._cycle_job = None  # after() job id

        # Side quick commands
        cmd_frame = ttk.LabelFrame(side, text="扩展命令")
        cmd_frame.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self._cmd_list = tk.Listbox(cmd_frame, height=14)
        self._cmd_list.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self._cmd_list.bind("<Double-1>", lambda e: self._send_selected_command())

        self._cmd_input_var = tk.StringVar()
        ttk.Entry(cmd_frame, textvariable=self._cmd_input_var).pack(fill=tk.X, padx=4, pady=(0, 4))

        cmd_btns = ttk.Frame(cmd_frame)
        cmd_btns.pack(fill=tk.X, padx=4, pady=(0, 4))
        ttk.Button(cmd_btns, text="添加", command=self._add_quick_command).pack(side=tk.LEFT)
        ttk.Button(cmd_btns, text="删除", command=self._remove_quick_command).pack(side=tk.LEFT, padx=4)
        ttk.Button(cmd_btns, text="发送", command=self._send_selected_command).pack(side=tk.LEFT)
        ttk.Button(cmd_btns, text="保存", command=self._save_quick_commands).pack(side=tk.RIGHT)

        self._reload_quick_commands()

        self._refresh_ports()
        self._term_mode_changed()

    def _toggle_serial_wrap(self):
        """Toggle text wrapping in terminal."""
        if self._serial_wrap_var.get():
            self._serial_text.config(wrap=tk.WORD)
        else:
            self._serial_text.config(wrap=tk.NONE)

    def _term_insert(self, text, direction="rx"):
        """Insert text into terminal with optional timestamp and direction prefix."""
        if not text:
            return
        prefix_arrow = "<- " if direction == "rx" else "-> "

        if self._serial_ts_var.get():
            ts = time.strftime("%H:%M:%S.") + f"{int(time.time()*1000)%1000:03d}"
            line_prefix = f"[{ts}] {prefix_arrow}"
        else:
            line_prefix = prefix_arrow

        # Build output: add prefix at line starts (optimized)
        parts = []
        if self._ts_need_prefix:
            parts.append(line_prefix)
            self._ts_need_prefix = False
        # Split by newline, add prefix after each newline
        lines = text.split("\n")
        for i, line in enumerate(lines):
            parts.append(line)
            if i < len(lines) - 1:
                parts.append("\n")
                parts.append(line_prefix)
        # If text ends with newline, next insert needs prefix
        if text.endswith("\n"):
            self._ts_need_prefix = True
            # Remove trailing prefix we just added
            if parts and parts[-1] == line_prefix:
                parts.pop()

        out = "".join(parts)
        self._serial_text.insert(tk.END, out)

        # Limit terminal buffer to avoid slowdown
        line_count = int(self._serial_text.index("end-1c").split(".")[0])
        if line_count > 5000:
            self._serial_text.delete("1.0", f"{line_count - 3000}.0")

        self._serial_text.see(tk.END)

    def _term_mode_changed(self):
        """Show/hide serial vs TCP client widgets based on terminal mode."""
        mode = self._term_mode_var.get()
        if mode == "tcp_client":
            for w in self._serial_widgets:
                w.pack_forget()
            for w in self._tcp_widgets:
                w.pack(side=tk.LEFT, padx=2)
        else:
            for w in self._tcp_widgets:
                w.pack_forget()
            for w in self._serial_widgets:
                w.pack(side=tk.LEFT, padx=2)

    def _refresh_ports(self):
        ports = SerialEngine.list_ports()
        self._com_cb["values"] = ports
        if ports and not self._com_var.get():
            self._com_var.set(ports[0])

    def _stop_cycle_send(self):
        if self._cycle_job is not None:
            self.after_cancel(self._cycle_job)
            self._cycle_job = None
            self._cycle_btn.config(text="周期发送")

    def _toggle_serial(self):
        mode = self._term_mode_var.get()
        if mode == "tcp_client":
            if self.tcp_client_engine.is_open:
                self._stop_cycle_send()
                self.tcp_client_engine.close()
                self._serial_btn.config(text="连接")
            else:
                host = self._tcp_host_var.get().strip()
                try:
                    port = int(self._tcp_port_var.get())
                except ValueError:
                    self._log_q.put("端口必须是整数")
                    return
                if not host:
                    self._log_q.put("请输入服务器 IP")
                    return
                if self.tcp_client_engine.open(host, port):
                    self._serial_btn.config(text="断开")
                    self.cfg.tcp_client_host = host
                    self.cfg.tcp_client_port = port
                    self.cfg.terminal_mode = "tcp_client"
                    self.cfg.save()
        else:
            if self.serial_engine.is_open:
                self._stop_cycle_send()
                self.serial_engine.close()
                self._serial_btn.config(text="打开")
            else:
                port = self._com_var.get()
                baud = int(self._baud_var.get())
                if self.serial_engine.open(port, baud):
                    self._serial_btn.config(text="关闭")
                    self.cfg.serial_port = port
                    self.cfg.serial_baud = baud
                    self.cfg.terminal_mode = "serial"
                    self.cfg.save()

    def _serial_send(self):
        txt = self._serial_send_var.get()
        if not txt:
            return
        if self._serial_send_hex.get():
            try:
                data = bytes.fromhex(txt.replace(" ", ""))
            except ValueError:
                self._log_q.put("HEX 格式错误")
                return
            display_txt = " ".join(f"{b:02X}" for b in data) + "\n"
        else:
            data = (txt + "\r\n").encode("utf-8")
            display_txt = txt + "\n"

        if self.tcp_client_engine.is_open:
            self.tcp_client_engine.write(data)
        elif self.serial_engine.is_open:
            self.serial_engine.write(data)
        else:
            self._log_q.put("未连接，请先打开串口或TCP连接")
            return

        if self._serial_show_send_var.get():
            self._term_insert(display_txt, direction="tx")
        self._serial_send_var.set("")

    def _toggle_cycle_send(self):
        if self._cycle_job is not None:
            self.after_cancel(self._cycle_job)
            self._cycle_job = None
            self._cycle_btn.config(text="周期发送")
        else:
            txt = self._serial_send_var.get()
            if not txt:
                self._log_q.put("请输入要周期发送的内容")
                return
            try:
                interval = int(self._cycle_interval_var.get())
                if interval < 10:
                    interval = 10
            except ValueError:
                self._log_q.put("周期值必须是整数(ms)")
                return
            if self._serial_send_hex.get():
                try:
                    self._cycle_data = bytes.fromhex(txt.replace(" ", ""))
                    self._cycle_display = " ".join(f"{b:02X}" for b in self._cycle_data) + "\n"
                except ValueError:
                    self._log_q.put("HEX 格式错误")
                    return
            else:
                self._cycle_data = (txt + "\r\n").encode("utf-8")
                self._cycle_display = txt + "\n"
            self._cycle_btn.config(text="停止")
            self._cycle_send_tick(interval)

    def _cycle_send_tick(self, interval):
        if self.tcp_client_engine.is_open:
            self.tcp_client_engine.write(self._cycle_data)
        elif self.serial_engine.is_open:
            self.serial_engine.write(self._cycle_data)
        else:
            self.after_cancel(self._cycle_job)
            self._cycle_job = None
            self._cycle_btn.config(text="周期发送")
            self._log_q.put("连接已断开，周期发送已停止")
            return
        if self._serial_show_send_var.get():
            self._term_insert(self._cycle_display, direction="tx")
        self._cycle_job = self.after(interval, self._cycle_send_tick, interval)

    def _reload_quick_commands(self):
        self._cmd_list.delete(0, tk.END)
        for cmd in self.cfg.quick_commands:
            self._cmd_list.insert(tk.END, cmd)

    def _add_quick_command(self):
        cmd = self._cmd_input_var.get().strip()
        if not cmd:
            return
        self._cmd_list.insert(tk.END, cmd)
        self._cmd_input_var.set("")

    def _remove_quick_command(self):
        selected = self._cmd_list.curselection()
        if not selected:
            return
        self._cmd_list.delete(selected[0])

    def _send_selected_command(self):
        selected = self._cmd_list.curselection()
        if not selected:
            return
        self._serial_send_var.set(self._cmd_list.get(selected[0]))
        self._serial_send()

    def _save_quick_commands(self):
        self.cfg.quick_commands = [self._cmd_list.get(i) for i in range(self._cmd_list.size())]
        self.cfg.save()
        self._log_q.put("扩展命令已保存")

    # ── Protocol Tab (Hex Custom Protocol) ──

    def _build_protocol_tab(self):
        f = ttk.Frame(self._nb)
        self._nb.add(f, text="  协议  ")

        # Main horizontal split: Protocol list (left) | Editor (right)
        main_pane = ttk.PanedWindow(f, orient=tk.HORIZONTAL)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        # ── Left: Protocol List ──
        list_frame = ttk.LabelFrame(main_pane, text="协议列表")
        main_pane.add(list_frame, weight=1)

        list_cols = ("name", "direction", "preview")
        self._plist_tree = ttk.Treeview(list_frame, columns=list_cols, show="headings",
                                        height=12, selectmode="browse")
        self._plist_tree.heading("name", text="名称")
        self._plist_tree.heading("direction", text="方向")
        self._plist_tree.heading("preview", text="HEX预览")
        self._plist_tree.column("name", width=80, minwidth=60)
        self._plist_tree.column("direction", width=50, minwidth=40)
        self._plist_tree.column("preview", width=160, minwidth=80)
        self._plist_tree.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self._plist_tree.bind("<<TreeviewSelect>>", self._plist_on_select)

        # List buttons
        plist_btns = ttk.Frame(list_frame)
        plist_btns.pack(fill=tk.X, padx=4, pady=2)
        ttk.Button(plist_btns, text="发送", width=5, command=self._plist_send).pack(side=tk.LEFT, padx=2)
        ttk.Button(plist_btns, text="编辑", width=5, command=self._plist_edit).pack(side=tk.LEFT, padx=2)
        ttk.Button(plist_btns, text="删除", width=5, command=self._plist_delete).pack(side=tk.LEFT, padx=2)

        # Cycle send for list item
        plist_bar2 = ttk.Frame(list_frame)
        plist_bar2.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(plist_bar2, text="周期(ms):").pack(side=tk.LEFT)
        self._plist_cycle_var = tk.StringVar(value="1000")
        ttk.Entry(plist_bar2, textvariable=self._plist_cycle_var, width=6).pack(side=tk.LEFT, padx=2)
        self._plist_cycle_btn = ttk.Button(plist_bar2, text="周期发送", command=self._plist_toggle_cycle)
        self._plist_cycle_btn.pack(side=tk.LEFT, padx=2)
        self._plist_cycle_job = None

        # Import/Export
        plist_bar3 = ttk.Frame(list_frame)
        plist_bar3.pack(fill=tk.X, padx=4, pady=(2, 4))
        ttk.Button(plist_bar3, text="导入JSON", command=self._proto_import).pack(side=tk.LEFT, padx=2)
        ttk.Button(plist_bar3, text="导出JSON", command=self._proto_export).pack(side=tk.LEFT, padx=2)

        # ── Right: Editor (vertical split: top = field editor, bottom = receive data) ──
        right_frame = ttk.Frame(main_pane)
        main_pane.add(right_frame, weight=3)

        vpane = ttk.PanedWindow(right_frame, orient=tk.VERTICAL)
        vpane.pack(fill=tk.BOTH, expand=True)

        # ── Top: Send Protocol Editor ──
        tx_frame = ttk.LabelFrame(vpane, text="协议编辑")
        vpane.add(tx_frame, weight=3)

        # Editor horizontal: field table + property
        hpane = ttk.PanedWindow(tx_frame, orient=tk.HORIZONTAL)
        hpane.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        left_ed = ttk.Frame(hpane)
        hpane.add(left_ed, weight=3)
        right_ed = ttk.Frame(hpane)
        hpane.add(right_ed, weight=2)

        # -- Field Treeview --
        cols = ("name", "type", "hex", "bytes")
        self._proto_tree = ttk.Treeview(left_ed, columns=cols, show="headings", height=6,
                                        selectmode="browse")
        self._proto_tree.heading("name", text="名称")
        self._proto_tree.heading("type", text="类型")
        self._proto_tree.heading("hex", text="值(HEX)")
        self._proto_tree.heading("bytes", text="字节数")
        self._proto_tree.column("name", width=80, minwidth=60)
        self._proto_tree.column("type", width=60, minwidth=50)
        self._proto_tree.column("hex", width=140, minwidth=80)
        self._proto_tree.column("bytes", width=50, minwidth=40)
        self._proto_tree.pack(fill=tk.BOTH, expand=True)
        self._proto_tree.bind("<<TreeviewSelect>>", self._proto_on_select)

        btn_bar = ttk.Frame(left_ed)
        btn_bar.pack(fill=tk.X, pady=2)
        ttk.Button(btn_bar, text="添加字段", width=8, command=self._proto_add_field).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_bar, text="删除", width=5, command=self._proto_del_field).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_bar, text="↑", width=3, command=lambda: self._proto_move_field(-1)).pack(side=tk.LEFT, padx=1)
        ttk.Button(btn_bar, text="↓", width=3, command=lambda: self._proto_move_field(1)).pack(side=tk.LEFT, padx=1)
        ttk.Button(btn_bar, text="默认", width=5, command=self._proto_reset_default).pack(side=tk.RIGHT, padx=2)

        # -- Property Panel --
        prop = ttk.LabelFrame(right_ed, text="字段属性")
        prop.pack(fill=tk.BOTH, expand=True, padx=2)

        r = 0
        ttk.Label(prop, text="名称:").grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_name_var = tk.StringVar()
        ttk.Entry(prop, textvariable=self._pf_name_var, width=16).grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        ttk.Label(prop, text="类型:").grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_type_var = tk.StringVar()
        self._pf_type_cb = ttk.Combobox(prop, textvariable=self._pf_type_var, width=14,
                                        values=list(FIELD_TYPE_LABELS.values()), state="readonly")
        self._pf_type_cb.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)
        self._pf_type_cb.bind("<<ComboboxSelected>>", self._proto_type_changed)

        r += 1
        self._pf_hex_lbl = ttk.Label(prop, text="值(HEX):")
        self._pf_hex_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_hex_var = tk.StringVar()
        self._pf_hex_entry = ttk.Entry(prop, textvariable=self._pf_hex_var, width=24)
        self._pf_hex_entry.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        self._pf_dtype_lbl = ttk.Label(prop, text="数据类型:")
        self._pf_dtype_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        data_type_labels = ["raw(HEX)", "uint8", "int8", "uint16", "int16", "uint32", "int32", "float"]
        self._pf_dtype_var = tk.StringVar(value="raw(HEX)")
        self._pf_dtype_cb = ttk.Combobox(prop, textvariable=self._pf_dtype_var,
                                         values=data_type_labels, width=14, state="readonly")
        self._pf_dtype_cb.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)
        self._pf_dtype_cb.bind("<<ComboboxSelected>>", self._proto_dtype_changed)

        r += 1
        self._pf_endian_lbl = ttk.Label(prop, text="字节序:")
        self._pf_endian_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_endian_var = tk.StringVar(value="le")
        self._pf_endian_cb = ttk.Combobox(prop, textvariable=self._pf_endian_var,
                                          values=["le (小端)", "be (大端)"], width=14, state="readonly")
        self._pf_endian_cb.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        self._pf_decval_lbl = ttk.Label(prop, text="十进制值:")
        self._pf_decval_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_decval_var = tk.StringVar()
        self._pf_decval_entry = ttk.Entry(prop, textvariable=self._pf_decval_var, width=16)
        self._pf_decval_entry.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        self._pf_len_lbl = ttk.Label(prop, text="覆盖字段:")
        self._pf_len_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_len_cover_var = tk.StringVar(value="all")
        self._pf_len_cover = ttk.Entry(prop, textvariable=self._pf_len_cover_var, width=16)
        self._pf_len_cover.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        self._pf_len_size_lbl = ttk.Label(prop, text="长度字节数:")
        self._pf_len_size_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_len_size_var = tk.StringVar(value="1")
        self._pf_len_size = ttk.Combobox(prop, textvariable=self._pf_len_size_var,
                                         values=["1", "2"], width=6, state="readonly")
        self._pf_len_size.grid(row=r, column=1, padx=4, pady=2, sticky=tk.W)

        r += 1
        self._pf_len_endian_lbl = ttk.Label(prop, text="长度字节序:")
        self._pf_len_endian_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_len_endian_var = tk.StringVar(value="le")
        self._pf_len_endian = ttk.Combobox(prop, textvariable=self._pf_len_endian_var,
                                           values=["le", "be"], width=6, state="readonly")
        self._pf_len_endian.grid(row=r, column=1, padx=4, pady=2, sticky=tk.W)

        r += 1
        self._pf_crc_method_lbl = ttk.Label(prop, text="校验方法:")
        self._pf_crc_method_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        crc_labels = [v[0] for v in CRC_METHODS.values()]
        self._pf_crc_method_var = tk.StringVar()
        self._pf_crc_method = ttk.Combobox(prop, textvariable=self._pf_crc_method_var,
                                           values=crc_labels, width=18, state="readonly")
        self._pf_crc_method.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        self._pf_crc_cover_lbl = ttk.Label(prop, text="校验覆盖:")
        self._pf_crc_cover_lbl.grid(row=r, column=0, padx=4, pady=2, sticky=tk.E)
        self._pf_crc_cover_var = tk.StringVar(value="all")
        self._pf_crc_cover = ttk.Entry(prop, textvariable=self._pf_crc_cover_var, width=16)
        self._pf_crc_cover.grid(row=r, column=1, padx=4, pady=2, sticky=tk.EW)

        r += 1
        ttk.Label(prop, text="(覆盖: \"all\" 或 \"0,2,3\")",
                  foreground="gray", font=("", 8)).grid(row=r, column=0, columnspan=2, padx=4, pady=1)

        r += 1
        self._pf_apply_btn = ttk.Button(prop, text="应用修改", command=self._proto_apply_field)
        self._pf_apply_btn.grid(row=r, column=0, columnspan=2, padx=4, pady=4)

        prop.columnconfigure(1, weight=1)

        self._pf_data_widgets = [self._pf_dtype_lbl, self._pf_dtype_cb,
                                 self._pf_endian_lbl, self._pf_endian_cb,
                                 self._pf_decval_lbl, self._pf_decval_entry]
        self._pf_len_widgets = [self._pf_len_lbl, self._pf_len_cover,
                                self._pf_len_size_lbl, self._pf_len_size,
                                self._pf_len_endian_lbl, self._pf_len_endian]
        self._pf_crc_widgets = [self._pf_crc_method_lbl, self._pf_crc_method,
                                self._pf_crc_cover_lbl, self._pf_crc_cover]

        # ── Preview + Save to list ──
        preview_frame = ttk.Frame(tx_frame)
        preview_frame.pack(fill=tk.X, padx=4, pady=2)

        ttk.Label(preview_frame, text="预览:").pack(side=tk.LEFT, padx=2)
        self._proto_preview_var = tk.StringVar(value="")
        ttk.Label(preview_frame, textvariable=self._proto_preview_var,
                  font=("Consolas", 10), foreground="blue").pack(side=tk.LEFT, fill=tk.X, expand=True, padx=4)

        save_bar = ttk.Frame(tx_frame)
        save_bar.pack(fill=tk.X, padx=4, pady=(0, 4))
        ttk.Label(save_bar, text="协议名:").pack(side=tk.LEFT, padx=2)
        self._proto_name_var = tk.StringVar(value="协议1")
        ttk.Entry(save_bar, textvariable=self._proto_name_var, width=12).pack(side=tk.LEFT, padx=2)

        ttk.Label(save_bar, text="方向:").pack(side=tk.LEFT, padx=(8, 2))
        self._proto_dir_var = tk.StringVar(value="发送")
        ttk.Combobox(save_bar, textvariable=self._proto_dir_var,
                     values=["发送", "接收"], width=6, state="readonly").pack(side=tk.LEFT, padx=2)

        ttk.Button(save_bar, text="保存到列表", command=self._proto_save_to_list).pack(side=tk.LEFT, padx=8)
        ttk.Button(save_bar, text="发送当前", command=self._proto_send).pack(side=tk.RIGHT, padx=2)

        # ── Bottom: Receive Data Panel ──
        rx_frame = ttk.LabelFrame(vpane, text="接收数据")
        vpane.add(rx_frame, weight=2)

        rx_bar = ttk.Frame(rx_frame)
        rx_bar.pack(fill=tk.X, padx=4, pady=4)

        self._rx_enabled_var = tk.BooleanVar(value=self.cfg.rx_protocol_enabled)
        ttk.Checkbutton(rx_bar, text="启用解析", variable=self._rx_enabled_var,
                        command=self._rx_toggle).pack(side=tk.LEFT, padx=4)
        ttk.Button(rx_bar, text="绘制曲线", command=self._rx_open_chart).pack(side=tk.RIGHT, padx=4)
        ttk.Button(rx_bar, text="清空", command=self._rx_clear_data).pack(side=tk.RIGHT, padx=4)

        # Rx data table: show parsed values with plot checkbox
        rx_cols = ("plot", "protocol", "field", "hex", "value", "time")
        self._rx_tree = ttk.Treeview(rx_frame, columns=rx_cols, show="headings", height=5)
        self._rx_tree.heading("plot", text="绘图")
        self._rx_tree.heading("protocol", text="协议")
        self._rx_tree.heading("field", text="字段")
        self._rx_tree.heading("hex", text="HEX")
        self._rx_tree.heading("value", text="值")
        self._rx_tree.heading("time", text="时间")
        self._rx_tree.column("plot", width=40, minwidth=35, anchor=tk.CENTER)
        self._rx_tree.column("protocol", width=70, minwidth=50)
        self._rx_tree.column("field", width=70, minwidth=50)
        self._rx_tree.column("hex", width=100, minwidth=60)
        self._rx_tree.column("value", width=100, minwidth=60)
        self._rx_tree.column("time", width=90, minwidth=70)
        self._rx_tree.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 4))
        self._rx_tree.bind("<ButtonRelease-1>", self._rx_toggle_plot)

        # ── Initialize state ──
        self._proto_fields = _default_protocol()  # current editor fields
        self._proto_list = list(self.cfg.protocol_list) if hasattr(self.cfg, 'protocol_list') else []
        self._rx_buffer = bytearray()
        self._rx_parsers = []
        self._rx_plot_checked = set()  # set of tree iids that are checked for plotting
        self._rx_history = {}  # {iid: [(timestamp, value), ...]}
        self._rx_chart_win = None
        self._plist_cycle_job = None
        self._editing_idx = None
        self._proto_refresh_tree()
        self._proto_update_preview()
        self._plist_refresh()
        self._rx_rebuild_tree()

    # ── Protocol field management ──

    def _proto_refresh_tree(self):
        """Refresh the send protocol field treeview."""
        for item in self._proto_tree.get_children():
            self._proto_tree.delete(item)
        assembled, ranges = _assemble_protocol(self._proto_fields)
        for i, f in enumerate(self._proto_fields):
            ftype_label = FIELD_TYPE_LABELS.get(f["type"], f["type"])
            start, end = ranges[i] if i < len(ranges) else (0, 0)
            field_bytes = assembled[start:end]
            hex_display = " ".join(f"{b:02X}" for b in field_bytes) if field_bytes else ""
            byte_count = len(field_bytes)
            if f["type"] in (FIELD_LENGTH, FIELD_CRC):
                hex_display = f"({hex_display})" if hex_display else "(auto)"
            self._proto_tree.insert("", tk.END, iid=str(i),
                                    values=(f["name"], ftype_label, hex_display, byte_count))

    def _proto_update_preview(self):
        """Update the hex preview."""
        try:
            assembled, _ = _assemble_protocol(self._proto_fields)
            hex_str = " ".join(f"{b:02X}" for b in assembled)
            self._proto_preview_var.set(hex_str if hex_str else "(空)")
        except Exception as e:
            self._proto_preview_var.set(f"错误: {e}")

    def _proto_on_select(self, event=None):
        """Load selected field into property editor."""
        sel = self._proto_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        if idx >= len(self._proto_fields):
            return
        f = self._proto_fields[idx]
        self._pf_name_var.set(f["name"])
        type_label = FIELD_TYPE_LABELS.get(f["type"], "数据")
        self._pf_type_var.set(type_label)
        self._pf_hex_var.set(f.get("hex", ""))
        dtype = f.get("data_type", "raw")
        dtype_label_map = {"raw": "raw(HEX)", "uint8": "uint8", "int8": "int8",
                           "uint16": "uint16", "int16": "int16", "uint32": "uint32",
                           "int32": "int32", "float": "float"}
        self._pf_dtype_var.set(dtype_label_map.get(dtype, "raw(HEX)"))
        endian = f.get("data_endian", "le")
        self._pf_endian_var.set("le (小端)" if endian == "le" else "be (大端)")
        self._pf_decval_var.set(f.get("data_value", ""))
        self._pf_len_cover_var.set(f.get("length_cover", "all"))
        self._pf_len_size_var.set(str(f.get("length_size", 1)))
        self._pf_len_endian_var.set(f.get("length_endian", "le"))
        crc_key = f.get("crc_method", "SUM8")
        if crc_key in CRC_METHODS:
            self._pf_crc_method_var.set(CRC_METHODS[crc_key][0])
        self._pf_crc_cover_var.set(f.get("crc_cover", "all"))
        self._proto_update_prop_visibility(f["type"])

    def _proto_type_changed(self, event=None):
        label = self._pf_type_var.get()
        ftype = FIELD_DATA
        for k, v in FIELD_TYPE_LABELS.items():
            if v == label:
                ftype = k
                break
        self._proto_update_prop_visibility(ftype)

    def _proto_dtype_changed(self, event=None):
        dtype_label = self._pf_dtype_var.get()
        is_raw = dtype_label == "raw(HEX)"
        self._pf_hex_entry.config(state="normal" if is_raw else "disabled")
        self._pf_decval_entry.config(state="disabled" if is_raw else "normal")
        self._pf_endian_cb.config(state="disabled" if is_raw or dtype_label in ("uint8", "int8") else "readonly")

    def _proto_update_prop_visibility(self, ftype):
        if ftype == FIELD_CONST:
            self._pf_hex_entry.config(state="normal")
        elif ftype == FIELD_DATA:
            dtype_label = self._pf_dtype_var.get()
            is_raw = dtype_label == "raw(HEX)"
            self._pf_hex_entry.config(state="normal" if is_raw else "disabled")
        else:
            self._pf_hex_entry.config(state="disabled")

        for w in self._pf_data_widgets:
            try:
                if ftype == FIELD_DATA:
                    w.configure(state="readonly" if isinstance(w, ttk.Combobox) else "normal")
                else:
                    w.configure(state="disabled")
            except tk.TclError:
                pass
        if ftype == FIELD_DATA:
            self._proto_dtype_changed()

        for w in self._pf_len_widgets:
            try:
                if ftype == FIELD_LENGTH:
                    w.configure(state="readonly" if isinstance(w, ttk.Combobox) else "normal")
                else:
                    w.configure(state="disabled")
            except tk.TclError:
                pass

        for w in self._pf_crc_widgets:
            try:
                if ftype == FIELD_CRC:
                    w.configure(state="readonly" if isinstance(w, ttk.Combobox) else "normal")
                else:
                    w.configure(state="disabled")
            except tk.TclError:
                pass

    def _proto_apply_field(self):
        sel = self._proto_tree.selection()
        if not sel:
            self._log_q.put("请先选择一个字段")
            return
        idx = int(sel[0])
        if idx >= len(self._proto_fields):
            return
        f = self._proto_fields[idx]
        f["name"] = self._pf_name_var.get().strip() or f["name"]

        label = self._pf_type_var.get()
        for k, v in FIELD_TYPE_LABELS.items():
            if v == label:
                f["type"] = k
                break

        dtype_label = self._pf_dtype_var.get()
        dtype_map = {"raw(HEX)": "raw", "uint8": "uint8", "int8": "int8",
                     "uint16": "uint16", "int16": "int16", "uint32": "uint32",
                     "int32": "int32", "float": "float"}
        f["data_type"] = dtype_map.get(dtype_label, "raw")
        endian_str = self._pf_endian_var.get()
        f["data_endian"] = "be" if "be" in endian_str else "le"
        f["data_value"] = self._pf_decval_var.get().strip()

        hex_val = self._pf_hex_var.get().strip().replace(" ", "")
        if f["type"] == FIELD_CONST:
            try:
                bytes.fromhex(hex_val)
                f["hex"] = hex_val
            except ValueError:
                self._log_q.put("HEX格式错误")
                return
        elif f["type"] == FIELD_DATA:
            if f["data_type"] == "raw":
                try:
                    bytes.fromhex(hex_val)
                    f["hex"] = hex_val
                except ValueError:
                    self._log_q.put("HEX格式错误")
                    return
            else:
                dec_val = f["data_value"]
                if not dec_val:
                    dec_val = "0"
                    f["data_value"] = "0"
                try:
                    e = '<' if f["data_endian"] == 'le' else '>'
                    pack_map = {
                        "uint8": f"{e}B", "int8": f"{e}b",
                        "uint16": f"{e}H", "int16": f"{e}h",
                        "uint32": f"{e}I", "int32": f"{e}i",
                        "float": f"{e}f",
                    }
                    fmt = pack_map.get(f["data_type"])
                    if fmt:
                        val = float(dec_val) if f["data_type"] == "float" else int(dec_val)
                        raw = struct.pack(fmt, val)
                    else:
                        raw = b"\x00"
                    f["hex"] = raw.hex()
                    self._pf_hex_var.set(" ".join(f"{b:02X}" for b in raw))
                    self._pf_decval_var.set(dec_val)
                except (ValueError, struct.error) as err:
                    self._log_q.put(f"数值转换错误: {err}")
                    return

        f["length_cover"] = self._pf_len_cover_var.get().strip() or "all"
        try:
            f["length_size"] = int(self._pf_len_size_var.get())
        except ValueError:
            f["length_size"] = 1
        f["length_endian"] = self._pf_len_endian_var.get() or "le"

        crc_label = self._pf_crc_method_var.get()
        for k, (lbl, _) in CRC_METHODS.items():
            if lbl == crc_label:
                f["crc_method"] = k
                break
        f["crc_cover"] = self._pf_crc_cover_var.get().strip() or "all"

        self._proto_refresh_tree()
        self._proto_update_preview()
        self._proto_tree.selection_set(str(idx))

    def _proto_add_field(self):
        self._proto_fields.append(_default_field())
        self._proto_refresh_tree()
        self._proto_update_preview()

    def _proto_del_field(self):
        sel = self._proto_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        if idx < len(self._proto_fields):
            del self._proto_fields[idx]
            self._proto_refresh_tree()
            self._proto_update_preview()

    def _proto_move_field(self, direction):
        sel = self._proto_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        new_idx = idx + direction
        if 0 <= new_idx < len(self._proto_fields):
            self._proto_fields[idx], self._proto_fields[new_idx] = \
                self._proto_fields[new_idx], self._proto_fields[idx]
            self._proto_refresh_tree()
            self._proto_update_preview()
            self._proto_tree.selection_set(str(new_idx))

    def _proto_reset_default(self):
        self._proto_fields = _default_protocol()
        self._proto_refresh_tree()
        self._proto_update_preview()

    # ── Protocol send ──

    def _proto_send_data(self, fields):
        """Assemble and send protocol data. Returns assembled bytes or None."""
        try:
            assembled, _ = _assemble_protocol(fields)
        except Exception as e:
            self._log_q.put(f"组包错误: {e}")
            return None
        if not assembled:
            self._log_q.put("组包为空")
            return None

        if self.tcp_client_engine.is_open:
            self.tcp_client_engine.write(assembled)
        elif self.serial_engine.is_open:
            self.serial_engine.write(assembled)
        else:
            self._log_q.put("未连接，请先在终端页打开串口或TCP连接")
            return None

        hex_str = " ".join(f"{b:02X}" for b in assembled)
        if self._serial_show_send_var.get():
            self._term_insert(hex_str + "\n", direction="tx")
        return assembled

    def _proto_send(self):
        """Send from editor."""
        self._proto_send_data(self._proto_fields)

    # ── Protocol List management ──

    def _plist_refresh(self):
        """Refresh protocol list treeview."""
        for item in self._plist_tree.get_children():
            self._plist_tree.delete(item)
        for i, p in enumerate(self._proto_list):
            name = p.get("name", f"协议{i}")
            direction = p.get("direction", "发送")
            try:
                assembled, _ = _assemble_protocol(p.get("fields", []))
                preview = " ".join(f"{b:02X}" for b in assembled[:16])
                if len(assembled) > 16:
                    preview += "..."
            except Exception:
                preview = "(错误)"
            self._plist_tree.insert("", tk.END, iid=str(i),
                                    values=(name, direction, preview))

    def _proto_save_to_list(self):
        """Save current editor protocol to the list."""
        import copy
        name = self._proto_name_var.get().strip() or f"协议{len(self._proto_list) + 1}"
        direction = self._proto_dir_var.get()
        entry = {
            "name": name,
            "direction": direction,
            "fields": copy.deepcopy(self._proto_fields),
        }
        if self._editing_idx is not None and 0 <= self._editing_idx < len(self._proto_list):
            self._proto_list[self._editing_idx] = entry
            self._editing_idx = None
            self._log_q.put(f"协议 \"{name}\" 已更新")
        else:
            self._proto_list.append(entry)
            self._log_q.put(f"协议 \"{name}\" 已保存到列表")
        self._plist_refresh()
        self._rx_rebuild_tree()
        self._proto_save_config()

    def _plist_on_select(self, event=None):
        """Load selected list item into editor for viewing/editing."""
        pass  # Just select, don't auto-load to avoid losing edits

    def _plist_edit(self):
        """Load selected protocol into editor."""
        import copy
        sel = self._plist_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        if idx >= len(self._proto_list):
            return
        p = self._proto_list[idx]
        self._proto_fields = copy.deepcopy(p.get("fields", _default_protocol()))
        self._proto_name_var.set(p.get("name", ""))
        self._proto_dir_var.set(p.get("direction", "发送"))
        self._proto_refresh_tree()
        self._proto_update_preview()
        # Store edit index so save can update existing
        self._editing_idx = idx
        self._log_q.put(f"已加载协议 \"{p.get('name', '')}\" 到编辑器")

    def _plist_delete(self):
        sel = self._plist_tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        if idx < len(self._proto_list):
            name = self._proto_list[idx].get("name", "")
            del self._proto_list[idx]
            self._plist_refresh()
            self._rx_rebuild_tree()
            self._proto_save_config()
            self._log_q.put(f"已删除协议 \"{name}\"")

    def _plist_send(self):
        """Send selected protocol from list."""
        sel = self._plist_tree.selection()
        if not sel:
            self._log_q.put("请先选择一个协议")
            return
        idx = int(sel[0])
        if idx >= len(self._proto_list):
            return
        p = self._proto_list[idx]
        self._proto_send_data(p.get("fields", []))

    def _plist_toggle_cycle(self):
        if self._plist_cycle_job is not None:
            self.after_cancel(self._plist_cycle_job)
            self._plist_cycle_job = None
            self._plist_cycle_btn.config(text="周期发送")
        else:
            sel = self._plist_tree.selection()
            if not sel:
                self._log_q.put("请先选择一个协议")
                return
            try:
                interval = int(self._plist_cycle_var.get())
                if interval < 10:
                    interval = 10
            except ValueError:
                self._log_q.put("周期值必须是整数(ms)")
                return
            self._plist_cycle_btn.config(text="停止")
            self._plist_cycle_tick(interval)

    def _plist_cycle_tick(self, interval):
        sel = self._plist_tree.selection()
        if not sel:
            self._plist_cycle_job = None
            self._plist_cycle_btn.config(text="周期发送")
            return
        idx = int(sel[0])
        if idx >= len(self._proto_list):
            self._plist_cycle_job = None
            self._plist_cycle_btn.config(text="周期发送")
            return
        p = self._proto_list[idx]
        result = self._proto_send_data(p.get("fields", []))
        if result is None:
            self._plist_cycle_job = None
            self._plist_cycle_btn.config(text="周期发送")
            return
        self._plist_cycle_job = self.after(interval, self._plist_cycle_tick, interval)

    def _proto_save_config(self):
        """Persist protocol list to config."""
        self.cfg.protocol_list = list(self._proto_list)
        self.cfg.rx_protocol_enabled = self._rx_enabled_var.get()
        self.cfg.save()

    def _proto_export(self):
        path = filedialog.asksaveasfilename(
            title="导出协议列表",
            filetypes=[("JSON", "*.json"), ("All Files", "*.*")],
            defaultextension=".json",
        )
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self._proto_list, f, ensure_ascii=False, indent=2)
        self._log_q.put(f"协议列表已导出: {os.path.basename(path)}")

    def _proto_import(self):
        path = filedialog.askopenfilename(
            title="导入协议列表",
            filetypes=[("JSON", "*.json"), ("All Files", "*.*")],
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, list):
                self._proto_list.extend(data)
                self._plist_refresh()
                self._rx_rebuild_tree()
                self._proto_save_config()
                self._log_q.put(f"已导入 {len(data)} 条协议")
        except Exception as e:
            self._log_q.put(f"导入失败: {e}")

    # ── Receive protocol parsing ──

    def _rx_toggle(self):
        if self._rx_enabled_var.get():
            self._rx_buffer.clear()
            self._log_q.put("接收解析已启用")
        else:
            self._log_q.put("接收解析已禁用")

    def _rx_rebuild_tree(self):
        """Rebuild the receive data treeview based on all protocols in list."""
        for item in self._rx_tree.get_children():
            self._rx_tree.delete(item)
        self._rx_parsers = []
        row = 0
        for pi, p in enumerate(self._proto_list):
            name = p.get("name", f"协议{pi}")
            fields = p.get("fields", [])
            # Find header (all const fields concatenated)
            header = b""
            data_fields = []
            for f in fields:
                if f["type"] == FIELD_CONST:
                    try:
                        header += bytes.fromhex(f["hex"].replace(" ", ""))
                    except ValueError:
                        pass
                elif f["type"] == FIELD_DATA:
                    data_fields.append(f)
            tree_ids = []
            direction = p.get("direction", "发送")
            for fi, f in enumerate(data_fields):
                iid = f"r{pi}_{fi}"
                plot_mark = "☑" if iid in self._rx_plot_checked else "☐"
                self._rx_tree.insert("", tk.END, iid=iid,
                                     values=(plot_mark,
                                             f"{name}({direction})" if fi == 0 else "",
                                             f["name"], "", "", ""))
                tree_ids.append(iid)
            if data_fields:
                self._rx_parsers.append({
                    "name": name,
                    "header": header,
                    "fields": fields,
                    "data_fields": data_fields,
                    "tree_ids": tree_ids,
                })
            row += len(data_fields)

    def _rx_clear_data(self):
        self._rx_buffer.clear()
        self._rx_history.clear()
        self._rx_rebuild_tree()

    def _rx_toggle_plot(self, event=None):
        """Toggle plot checkbox when clicking the 'plot' column."""
        region = self._rx_tree.identify_region(event.x, event.y)
        if region != "cell":
            return
        col = self._rx_tree.identify_column(event.x)
        if col != "#1":  # "plot" is the first column
            return
        iid = self._rx_tree.identify_row(event.y)
        if not iid:
            return
        if iid in self._rx_plot_checked:
            self._rx_plot_checked.discard(iid)
            self._rx_tree.set(iid, "plot", "☐")
        else:
            self._rx_plot_checked.add(iid)
            self._rx_tree.set(iid, "plot", "☑")

    def _rx_open_chart(self):
        """Open a chart window to plot checked fields."""
        checked = [iid for iid in self._rx_plot_checked if iid in self._rx_history and self._rx_history[iid]]
        if not checked:
            self._log_q.put("请先勾选要绘制的字段，并确保有解析数据")
            return
        if self._rx_chart_win is not None and self._rx_chart_win.winfo_exists():
            self._rx_chart_win.destroy()
        self._rx_chart_win = _ChartWindow(self, checked, self._rx_history, self._rx_tree)

    def _rx_feed_data(self, data: bytes):
        if not self._rx_enabled_var.get():
            return
        if not self._rx_parsers:
            return
        self._rx_buffer.extend(data)
        self._rx_try_parse()

    def _rx_try_parse(self):
        """Try to parse frames from all registered receive protocols."""
        for parser in self._rx_parsers:
            header = parser["header"]
            if not header:
                continue
            fields = parser["fields"]
            data_fields = parser["data_fields"]

            # Calculate frame size from all fields
            frame_size = 0
            for f in fields:
                if f["type"] == FIELD_CONST:
                    try:
                        frame_size += len(bytes.fromhex(f["hex"].replace(" ", "")))
                    except ValueError:
                        pass
                elif f["type"] == FIELD_DATA:
                    dtype = f.get("data_type", "raw")
                    if dtype == "raw":
                        try:
                            frame_size += len(bytes.fromhex(f.get("hex", "00").replace(" ", "")))
                        except ValueError:
                            frame_size += 1
                    else:
                        size_map = {"uint8": 1, "int8": 1, "uint16": 2, "int16": 2,
                                    "uint32": 4, "int32": 4, "float": 4}
                        frame_size += size_map.get(dtype, 1)
                elif f["type"] == FIELD_LENGTH:
                    frame_size += f.get("length_size", 1)
                elif f["type"] == FIELD_CRC:
                    method_key = f.get("crc_method", "SUM8")
                    if method_key in CRC_METHODS:
                        _, func = CRC_METHODS[method_key]
                        frame_size += len(func(b"\x00"))
                    else:
                        frame_size += 1

            if frame_size == 0:
                continue

            # Try to find and parse frames from the shared buffer
            while len(self._rx_buffer) >= frame_size:
                pos = bytes(self._rx_buffer).find(header)
                if pos < 0:
                    # No header found, keep last few bytes for partial match
                    keep = len(self._rx_buffer) - len(header) + 1
                    if keep > 0:
                        del self._rx_buffer[:keep]
                    break
                if pos > 0:
                    # Discard bytes before header
                    del self._rx_buffer[:pos]
                if len(self._rx_buffer) < frame_size:
                    break

                frame = bytes(self._rx_buffer[:frame_size])
                # Parse data fields
                offset = 0
                ts = time.strftime("%H:%M:%S.") + f"{int(time.time()*1000)%1000:03d}"
                di = 0  # data field index
                for f in fields:
                    if f["type"] == FIELD_CONST:
                        try:
                            offset += len(bytes.fromhex(f["hex"].replace(" ", "")))
                        except ValueError:
                            pass
                    elif f["type"] == FIELD_LENGTH:
                        offset += f.get("length_size", 1)
                    elif f["type"] == FIELD_DATA:
                        dtype = f.get("data_type", "raw")
                        endian = f.get("data_endian", "le")
                        if dtype == "raw":
                            try:
                                size = len(bytes.fromhex(f.get("hex", "00").replace(" ", "")))
                            except ValueError:
                                size = 1
                        else:
                            size_map = {"uint8": 1, "int8": 1, "uint16": 2, "int16": 2,
                                        "uint32": 4, "int32": 4, "float": 4}
                            size = size_map.get(dtype, 1)
                        if offset + size <= len(frame):
                            chunk = frame[offset:offset + size]
                            hex_str = " ".join(f"{b:02X}" for b in chunk)
                            # Decode value
                            if dtype != "raw":
                                e = '<' if endian == 'le' else '>'
                                pack_map = {
                                    "uint8": f"{e}B", "int8": f"{e}b",
                                    "uint16": f"{e}H", "int16": f"{e}h",
                                    "uint32": f"{e}I", "int32": f"{e}i",
                                    "float": f"{e}f",
                                }
                                fmt = pack_map.get(dtype)
                                if fmt and len(chunk) == size:
                                    try:
                                        value = str(struct.unpack(fmt, chunk)[0])
                                    except struct.error:
                                        value = hex_str
                                else:
                                    value = hex_str
                            else:
                                value = hex_str
                            # Update tree and history
                            if di < len(parser["tree_ids"]):
                                tid = parser["tree_ids"][di]
                                if self._rx_tree.exists(tid):
                                    self._rx_tree.set(tid, "hex", hex_str)
                                    self._rx_tree.set(tid, "value", value)
                                    self._rx_tree.set(tid, "time", ts)
                                    # Store history for chart
                                    try:
                                        num_val = float(value)
                                        if tid not in self._rx_history:
                                            self._rx_history[tid] = []
                                        self._rx_history[tid].append((time.time(), num_val))
                                        # Keep max 10000 points
                                        if len(self._rx_history[tid]) > 10000:
                                            self._rx_history[tid] = self._rx_history[tid][-5000:]
                                    except (ValueError, TypeError):
                                        pass
                        offset += size
                        di += 1
                    elif f["type"] == FIELD_CRC:
                        method_key = f.get("crc_method", "SUM8")
                        if method_key in CRC_METHODS:
                            _, func = CRC_METHODS[method_key]
                            offset += len(func(b"\x00"))
                        else:
                            offset += 1

                # Consume the parsed frame from buffer
                del self._rx_buffer[:frame_size]
        if len(self._rx_buffer) > 4096:
            del self._rx_buffer[:-2048]

    # ── Bridge Tab ──

    def _build_bridge_tab(self):
        f = ttk.Frame(self._nb)
        self._nb.add(f, text="  桥接  ")

        # Mode selector
        mode_f = ttk.LabelFrame(f, text="连接模式")
        mode_f.pack(fill=tk.X, padx=8, pady=4)

        self._mode_var = tk.StringVar(value=self.cfg.mode)
        r1 = ttk.Radiobutton(mode_f, text="中继桥接 (跨网络)", variable=self._mode_var,
                              value="relay", command=self._mode_changed)
        r1.grid(row=0, column=0, padx=8, pady=4, sticky=tk.W)
        r2 = ttk.Radiobutton(mode_f, text="直接连接 (同网络)", variable=self._mode_var,
                              value="direct", command=self._mode_changed)
        r2.grid(row=0, column=1, padx=8, pady=4, sticky=tk.W)

        # Relay settings
        self._relay_frame = ttk.LabelFrame(f, text="中继服务器")
        self._relay_frame.pack(fill=tk.X, padx=8, pady=4)

        ttk.Label(self._relay_frame, text="服务器:").grid(row=0, column=0, padx=4, pady=2, sticky=tk.E)
        self._relay_host_var = tk.StringVar(value=self.cfg.relay_host)
        ttk.Entry(self._relay_frame, textvariable=self._relay_host_var, width=30).grid(
            row=0, column=1, padx=4, pady=2, sticky=tk.EW)

        ttk.Label(self._relay_frame, text="中继端口:").grid(row=0, column=2, padx=4, pady=2, sticky=tk.E)
        self._relay_port_var = tk.StringVar(value=str(self.cfg.relay_port))
        ttk.Entry(self._relay_frame, textvariable=self._relay_port_var, width=8).grid(
            row=0, column=3, padx=4, pady=2, sticky=tk.W)

        ttk.Label(self._relay_frame, text="配对码:").grid(row=1, column=0, padx=4, pady=2, sticky=tk.E)
        self._relay_code_var = tk.StringVar(value=self.cfg.relay_code)
        ttk.Entry(self._relay_frame, textvariable=self._relay_code_var, width=20).grid(
            row=1, column=1, padx=4, pady=2, sticky=tk.W)

        # Protocol selector: WS or TCP
        ttk.Label(self._relay_frame, text="协议:").grid(row=2, column=0, padx=4, pady=2, sticky=tk.E)
        proto_f = ttk.Frame(self._relay_frame)
        proto_f.grid(row=2, column=1, columnspan=3, padx=4, pady=2, sticky=tk.W)
        self._relay_proto_var = tk.StringVar(value=self.cfg.relay_protocol)
        ttk.Radiobutton(proto_f, text="WebSocket", variable=self._relay_proto_var,
                        value="ws").pack(side=tk.LEFT, padx=4)
        ttk.Radiobutton(proto_f, text="TCP代理 (低延迟)", variable=self._relay_proto_var,
                        value="tcp").pack(side=tk.LEFT, padx=4)
        ttk.Label(proto_f, text="TCP端口:").pack(side=tk.LEFT, padx=(12, 2))
        self._relay_tcp_port_var = tk.StringVar(value=str(self.cfg.relay_tcp_port))
        ttk.Entry(proto_f, textvariable=self._relay_tcp_port_var, width=6).pack(side=tk.LEFT)

        self._relay_frame.columnconfigure(1, weight=1)

        # Direct settings
        self._direct_frame = ttk.LabelFrame(f, text="ESP32 地址")
        self._direct_frame.pack(fill=tk.X, padx=8, pady=4)

        ttk.Label(self._direct_frame, text="IP:端口:").grid(row=0, column=0, padx=4, pady=2, sticky=tk.E)
        self._esp_ip_var = tk.StringVar(value=self.cfg.esp_ip)
        ttk.Entry(self._direct_frame, textvariable=self._esp_ip_var, width=30).grid(
            row=0, column=1, padx=4, pady=2, sticky=tk.W)
        ttk.Label(self._direct_frame, text="无线调试端口:").grid(row=0, column=2, padx=4, pady=2, sticky=tk.E)
        self._esp_port_var = tk.StringVar(value=str(self.cfg.esp_port))
        ttk.Entry(self._direct_frame, textvariable=self._esp_port_var, width=8).grid(
            row=0, column=3, padx=4, pady=2, sticky=tk.W)
        ttk.Label(self._direct_frame, text="(如 192.168.1.100 或 192.168.1.100:3240)").grid(
            row=1, column=1, columnspan=3, padx=4, pady=2, sticky=tk.W)

        # Port settings
        port_f = ttk.LabelFrame(f, text="本地端口")
        port_f.pack(fill=tk.X, padx=8, pady=4)

        ttk.Label(port_f, text="DAP:").grid(row=0, column=0, padx=4, pady=2)
        self._dap_port_var = tk.StringVar(value=str(self.cfg.dap_port))
        ttk.Entry(port_f, textvariable=self._dap_port_var, width=8).grid(row=0, column=1, padx=2)

        ttk.Label(port_f, text="串口:").grid(row=0, column=2, padx=4, pady=2)
        self._serial_port_var = tk.StringVar(value=str(self.cfg.serial_tcp_port))
        ttk.Entry(port_f, textvariable=self._serial_port_var, width=8).grid(row=0, column=3, padx=2)

        ttk.Label(port_f, text="RTT:").grid(row=0, column=4, padx=4, pady=2)
        self._rtt_port_var = tk.StringVar(value=str(self.cfg.rtt_tcp_port))
        ttk.Entry(port_f, textvariable=self._rtt_port_var, width=8).grid(row=0, column=5, padx=2)

        # Connect button
        btn_f = ttk.Frame(f)
        btn_f.pack(fill=tk.X, padx=8, pady=8)
        self._bridge_btn = ttk.Button(btn_f, text="启动桥接", command=self._toggle_bridge)
        self._bridge_btn.pack(side=tk.LEFT)
        ttk.Button(btn_f, text="保存设置", command=self._save_bridge_settings).pack(side=tk.LEFT, padx=8)
        self._flash_btn = ttk.Button(btn_f, text="远程烧录", command=self._remote_flash)
        self._flash_btn.pack(side=tk.LEFT, padx=8)
        self._bridge_status = ttk.Label(btn_f, text="未连接", foreground="gray")
        self._bridge_status.pack(side=tk.LEFT, padx=12)

        # elaphureLink integration
        el_f = ttk.LabelFrame(f, text="elaphureLink / Keil 透传")
        el_f.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

        # Mode selector row: external exe vs integrated proxy
        mode_row = ttk.Frame(el_f)
        mode_row.pack(fill=tk.X, padx=4, pady=2)

        self._el_mode_var = tk.StringVar(value=self.cfg.elaphurelink_mode)
        ttk.Radiobutton(mode_row, text="外部 elaphureLink.exe",
                        variable=self._el_mode_var, value="external",
                        command=self._el_mode_changed).pack(side=tk.LEFT, padx=4)
        ttk.Radiobutton(mode_row, text="内置代理 (无需安装 elaphureLink)",
                        variable=self._el_mode_var, value="integrated",
                        command=self._el_mode_changed).pack(side=tk.LEFT, padx=4)

        ctrl_row = ttk.Frame(el_f)
        ctrl_row.pack(fill=tk.X, padx=4, pady=2)

        auto_exe = _find_elaphurelink_exe()
        self._el_path_var = tk.StringVar(value=self.cfg.elaphurelink_path or auto_exe)

        self._el_autostart_var = tk.BooleanVar(value=self.cfg.elaphurelink_autostart)
        ttk.Checkbutton(ctrl_row, text="桥接启动时自动开启透传", variable=self._el_autostart_var).pack(
            side=tk.LEFT, padx=4)
        self._el_btn = ttk.Button(ctrl_row, text="启动透传", command=self._toggle_elaphurelink)
        self._el_btn.pack(side=tk.LEFT, padx=4)
        self._el_status = ttk.Label(ctrl_row, text="未启动", foreground="gray")
        self._el_status.pack(side=tk.LEFT, padx=8)

        # Keil driver install button
        ttk.Button(ctrl_row, text="安装 Keil 驱动", command=self._install_keil_driver).pack(
            side=tk.RIGHT, padx=4)

        # External exe path row
        self._el_path_row = ttk.Frame(el_f)
        self._el_path_row.pack(fill=tk.X, padx=4, pady=2)
        ttk.Label(self._el_path_row, text="路径:").pack(side=tk.LEFT, padx=4)
        ttk.Entry(self._el_path_row, textvariable=self._el_path_var, width=40).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=4)
        ttk.Button(self._el_path_row, text="浏览", command=self._browse_elaphurelink).pack(side=tk.LEFT, padx=4)

        # Embed frame for elaphureLink window
        self._el_embed_frame = tk.Frame(el_f, bg="black")
        self._el_embed_frame.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self._el_hwnd = 0
        self._el_embed_frame.bind("<Configure>", self._on_el_frame_resize)

        # Apply initial mode visibility
        self._el_mode_changed()

        # Info text
        info = ttk.Label(f, text=(
            "中继模式: 通过中继服务器桥接，适合 PC 和设备不在同一网络\n"
            "直连模式: 直接 TCP 连接 ESP32，适合同一 WiFi/局域网\n"
            "透传: 勾选后启动桥接时自动开启 Keil 透传 (外部exe嵌入或内置代理)\n"
            "Keil 配置: 选择 elaphureLink 调试器, IP=127.0.0.1, 端口=上方 DAP 端口"
        ), justify=tk.LEFT, foreground="gray")
        info.pack(fill=tk.X, padx=8, pady=8, anchor=tk.NW)

        self._mode_changed()

    def _mode_changed(self):
        mode = self._mode_var.get()
        if mode == "relay":
            self._relay_frame.pack(fill=tk.X, padx=8, pady=4, after=self._relay_frame.master.winfo_children()[0])
            self._direct_frame.pack_forget()
            self._direct_frame.pack(fill=tk.X, padx=8, pady=4, after=self._relay_frame)
            # Show relay, hide direct
            for w in self._relay_frame.winfo_children():
                try:
                    w.configure(state="normal")
                except tk.TclError:
                    pass
            for w in self._direct_frame.winfo_children():
                try:
                    w.configure(state="disabled")
                except tk.TclError:
                    pass
        else:
            for w in self._direct_frame.winfo_children():
                try:
                    w.configure(state="normal")
                except tk.TclError:
                    pass
            for w in self._relay_frame.winfo_children():
                try:
                    w.configure(state="disabled")
                except tk.TclError:
                    pass

    def _browse_elaphurelink(self):
        path = filedialog.askopenfilename(
            title="选择 elaphureLink.Wpf.exe",
            filetypes=[("Executable", "*.exe"), ("All Files", "*.*")],
        )
        if path:
            self._el_path_var.set(path)

    def _remote_flash(self):
        """Open file dialog and start remote flash."""
        if not self.bridge_engine.is_running:
            messagebox.showwarning("远程烧录", "请先启动桥接")
            return
        hex_path = filedialog.askopenfilename(
            title="选择 HEX 固件文件",
            filetypes=[("Intel HEX", "*.hex"), ("All Files", "*.*")],
        )
        if not hex_path:
            return
        self._log_q.put(f"远程烧录: 选择文件 {os.path.basename(hex_path)}")
        self._flash_btn.config(state="disabled")

        def on_done(msg):
            self.after(0, lambda: self._flash_btn.config(state="normal"))

        self.bridge_engine.remote_flash(hex_path, log_callback=on_done)

    def _el_mode_changed(self):
        """Show/hide path row based on elaphureLink mode."""
        mode = self._el_mode_var.get()
        if mode == "external":
            self._el_path_row.pack(fill=tk.X, padx=4, pady=2)
            self._el_embed_frame.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        else:
            self._el_path_row.pack_forget()
            self._el_embed_frame.pack_forget()

    def _install_keil_driver(self):
        """Install elaphureLink RDDI driver into Keil MDK."""
        el_mode = self._el_mode_var.get()
        if el_mode == "external":
            exe = self._el_path_var.get().strip()
            if exe and os.path.isfile(exe):
                el_dir = os.path.dirname(exe)
            else:
                el_dir = ""
        else:
            # Find release dir relative to app
            if getattr(sys, 'frozen', False):
                base = sys._MEIPASS
            else:
                base = os.path.dirname(os.path.abspath(__file__))
            el_dir = os.path.join(base, 'elaphureLink_Windows_x64_release')

        if not el_dir or not os.path.isdir(el_dir):
            messagebox.showerror("安装 Keil 驱动",
                                 "找不到 elaphureLink 发行目录，请检查 exe 路径设置")
            return

        success, msg = _install_keil_elaphurelink_driver(
            el_dir, log_callback=lambda m: self._log_q.put(m))

        if success:
            messagebox.showinfo("安装 Keil 驱动", msg)
        else:
            # Auto-detection failed, offer manual TOOLS.INI selection
            if "请手动选择" in msg or "未找到" in msg:
                retry = messagebox.askyesno("安装 Keil 驱动",
                                            f"{msg}\n\n是否手动选择 TOOLS.INI 文件？")
                if retry:
                    ini_path = filedialog.askopenfilename(
                        title="选择 Keil TOOLS.INI 文件",
                        filetypes=[("INI Files", "*.INI"), ("All Files", "*.*")],
                    )
                    if ini_path:
                        success2, msg2 = _install_keil_elaphurelink_driver(
                            el_dir, log_callback=lambda m: self._log_q.put(m),
                            tools_ini_override=ini_path)
                        if success2:
                            messagebox.showinfo("安装 Keil 驱动", msg2)
                        else:
                            messagebox.showerror("安装 Keil 驱动", msg2)
            else:
                messagebox.showerror("安装 Keil 驱动", msg)

    def _toggle_elaphurelink(self):
        el_mode = self._el_mode_var.get()
        if el_mode == "integrated":
            if self._el_proxy and self._el_proxy.is_running:
                self._stop_el_proxy()
            else:
                self._start_el_proxy()
        else:
            if self._el_proc and self._el_proc.poll() is None:
                self._stop_elaphurelink()
            else:
                self._start_elaphurelink()

    def _start_el_proxy(self):
        """Start the integrated Python elaphureLink proxy."""
        if ElProxy is None:
            self._log_q.put("内置代理不可用: 未找到 el_proxy 模块")
            return
        if self._el_proxy and self._el_proxy.is_running:
            self._log_q.put("内置代理已在运行")
            return
        try:
            dap_port = int(self._dap_port_var.get())
        except ValueError:
            dap_port = DEFAULT_DAP_PORT
        self._el_proxy = ElProxy(log_callback=lambda m: self._log_q.put(f"[Proxy] {m}"))
        self._el_proxy.start("127.0.0.1", dap_port)
        self._el_btn.config(text="停止透传")
        self._el_status.config(text="内置代理运行中", foreground="green")
        self._log_q.put(f"内置 elaphureLink 代理已启动 (DAP @ 127.0.0.1:{dap_port})")

    def _stop_el_proxy(self):
        """Stop the integrated Python elaphureLink proxy."""
        if self._el_proxy:
            self._el_proxy.stop()
            self._el_proxy = None
        self._el_btn.config(text="启动透传")
        self._el_status.config(text="已停止", foreground="gray")
        self._log_q.put("内置代理已停止")

    def _start_elaphurelink(self):
        if self._el_proc and self._el_proc.poll() is None:
            self._log_q.put("elaphureLink 已在运行")
            return

        exe = self._el_path_var.get().strip()
        if not exe or not os.path.isfile(exe):
            self._log_q.put("未找到 elaphureLink.Wpf.exe，请检查路径设置")
            return

        # Auto-set deviceAddress to 127.0.0.1 before launching
        _set_elaphurelink_device_address("127.0.0.1")

        cwd = os.path.dirname(exe)
        try:
            self._el_proc = subprocess.Popen([exe], cwd=cwd)
            self._el_btn.config(text="停止透传")
            self._el_status.config(text="外部程序运行中", foreground="green")
            self._log_q.put(f"已启动 elaphureLink: {exe} (已自动设置地址为 127.0.0.1)")
            # Embed the window after a short delay
            self.after(1500, self._embed_elaphurelink)
        except Exception as e:
            self._el_proc = None
            self._log_q.put(f"启动 elaphureLink 失败: {e}")

    def _embed_elaphurelink(self):
        """Find the elaphureLink window and embed it into the tool."""
        if not self._el_proc or self._el_proc.poll() is not None:
            return
        if sys.platform != "win32":
            return
        try:
            hwnd = _find_hwnd_by_pid(self._el_proc.pid, timeout=5.0)
            if hwnd:
                parent_hwnd = self._el_embed_frame.winfo_id()
                _embed_hwnd(hwnd, parent_hwnd)
                self._el_hwnd = hwnd
                w = self._el_embed_frame.winfo_width()
                h = self._el_embed_frame.winfo_height()
                if w > 1 and h > 1:
                    _resize_embedded(hwnd, w, h)
                self._log_q.put("elaphureLink 窗口已嵌入")
            else:
                self._log_q.put("未找到 elaphureLink 窗口，将以独立窗口运行")
        except Exception as e:
            self._log_q.put(f"嵌入 elaphureLink 窗口失败: {e}")

    def _on_el_frame_resize(self, event):
        """Resize embedded elaphureLink window when frame resizes."""
        if self._el_hwnd and self._el_proc and self._el_proc.poll() is None:
            try:
                _resize_embedded(self._el_hwnd, event.width, event.height)
            except Exception:
                pass

    def _stop_elaphurelink(self):
        self._el_hwnd = 0
        if self._el_proc:
            if self._el_proc.poll() is None:
                try:
                    self._el_proc.terminate()
                except Exception:
                    pass
            self._el_proc = None
        self._el_btn.config(text="启动透传")
        self._el_status.config(text="已停止", foreground="gray")

    def _toggle_bridge(self):
        if self.bridge_engine.is_running:
            self.bridge_engine.stop()
            self._bridge_btn.config(text="启动桥接")
            self._bridge_status.config(text="已停止", foreground="gray")
        else:
            mode = self._mode_var.get()
            try:
                dap_port = int(self._dap_port_var.get())
                serial_port = int(self._serial_port_var.get())
                rtt_port = int(self._rtt_port_var.get())
                relay_port = int(self._relay_port_var.get())
                esp_port = int(self._esp_port_var.get())
                relay_tcp_port = int(self._relay_tcp_port_var.get())
            except ValueError:
                messagebox.showerror("参数错误", "端口必须是整数")
                return

            for value, label in (
                (dap_port, "DAP 端口"),
                (serial_port, "串口端口"),
                (rtt_port, "RTT 端口"),
                (relay_port, "中继端口"),
                (relay_tcp_port, "TCP中继端口"),
                (esp_port, "无线调试端口"),
            ):
                if value < 1 or value > 65535:
                    messagebox.showerror("参数错误", f"{label} 超出范围: {value}")
                    return

            # Save config
            self.cfg.mode = mode
            self.cfg.relay_host = self._relay_host_var.get().strip()
            self.cfg.relay_port = relay_port
            if self.cfg.relay_host:
                self.cfg.relay_url = f"ws://{self.cfg.relay_host}:{self.cfg.relay_port}"
            else:
                self.cfg.relay_url = ""
            self.cfg.relay_code = self._relay_code_var.get()
            self.cfg.relay_protocol = self._relay_proto_var.get()
            self.cfg.relay_tcp_port = relay_tcp_port
            self.cfg.esp_ip = self._esp_ip_var.get()
            self.cfg.esp_port = esp_port
            self.cfg.dap_port = dap_port
            self.cfg.serial_tcp_port = serial_port
            self.cfg.rtt_tcp_port = rtt_port
            self.cfg.elaphurelink_path = self._el_path_var.get().strip()
            self.cfg.elaphurelink_autostart = self._el_autostart_var.get()
            self.cfg.elaphurelink_mode = self._el_mode_var.get()
            self.cfg.save()

            if self.cfg.elaphurelink_autostart:
                el_mode = self._el_mode_var.get()
                if el_mode == "integrated":
                    self._start_el_proxy()
                else:
                    self._start_elaphurelink()

            self.bridge_engine = BridgeEngine(
                self._log_q, self._bridge_serial_q, self._rtt_rx_q)
            self.bridge_engine.start(
                mode=mode,
                relay_url=self.cfg.relay_url,
                relay_host=self.cfg.relay_host,
                relay_port=self.cfg.relay_port,
                code=self._relay_code_var.get(),
                esp_ip=self._esp_ip_var.get(),
                esp_port=self.cfg.esp_port,
                dap_port=dap_port,
                serial_port=serial_port,
                rtt_port=rtt_port,
                relay_protocol=self.cfg.relay_protocol,
                relay_tcp_port=self.cfg.relay_tcp_port,
            )
            self._bridge_btn.config(text="停止桥接")
            self._bridge_status.config(text="连接中...", foreground="orange")

    def _save_bridge_settings(self):
        try:
            self.cfg.mode = self._mode_var.get()
            self.cfg.relay_host = self._relay_host_var.get().strip()
            self.cfg.relay_port = int(self._relay_port_var.get())
            self.cfg.relay_code = self._relay_code_var.get().strip()
            self.cfg.relay_protocol = self._relay_proto_var.get()
            self.cfg.relay_tcp_port = int(self._relay_tcp_port_var.get())
            self.cfg.esp_ip = self._esp_ip_var.get().strip()
            self.cfg.esp_port = int(self._esp_port_var.get())
            self.cfg.dap_port = int(self._dap_port_var.get())
            self.cfg.serial_tcp_port = int(self._serial_port_var.get())
            self.cfg.rtt_tcp_port = int(self._rtt_port_var.get())
            self.cfg.elaphurelink_path = self._el_path_var.get().strip()
            self.cfg.elaphurelink_autostart = self._el_autostart_var.get()
            self.cfg.elaphurelink_mode = self._el_mode_var.get()
        except ValueError:
            messagebox.showerror("参数错误", "端口必须是整数")
            return

        for value, label in (
            (self.cfg.dap_port, "DAP 端口"),
            (self.cfg.serial_tcp_port, "串口端口"),
            (self.cfg.rtt_tcp_port, "RTT 端口"),
            (self.cfg.relay_port, "中继端口"),
            (self.cfg.relay_tcp_port, "TCP中继端口"),
            (self.cfg.esp_port, "无线调试端口"),
        ):
            if value < 1 or value > 65535:
                messagebox.showerror("参数错误", f"{label} 超出范围: {value}")
                return

        if self.cfg.relay_host:
            self.cfg.relay_url = f"ws://{self.cfg.relay_host}:{self.cfg.relay_port}"
        else:
            self.cfg.relay_url = ""
        self.cfg.quick_commands = [self._cmd_list.get(i) for i in range(self._cmd_list.size())]
        self.cfg.save()
        self._log_q.put("桥接参数已保存")

    # ── RTT Tab ──

    def _build_rtt_tab(self):
        f = ttk.Frame(self._nb)
        self._nb.add(f, text="  RTT  ")

        bar = ttk.Frame(f)
        bar.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(bar, text="RTT 上行输出 (通过桥接或 USB CDC)").pack(side=tk.LEFT)
        ttk.Button(bar, text="清屏", command=lambda: self._rtt_text.delete("1.0", tk.END)
                   ).pack(side=tk.RIGHT)

        self._rtt_text = scrolledtext.ScrolledText(f, wrap=tk.WORD, font=("Consolas", 10))
        self._rtt_text.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 2))

        # Send bar
        send_bar = ttk.Frame(f)
        send_bar.pack(fill=tk.X, padx=4, pady=(0, 4))
        self._rtt_send_var = tk.StringVar()
        entry = ttk.Entry(send_bar, textvariable=self._rtt_send_var)
        entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 4))
        entry.bind("<Return>", lambda e: self._rtt_send())
        ttk.Button(send_bar, text="发送", command=self._rtt_send).pack(side=tk.LEFT)

    def _rtt_send(self):
        txt = self._rtt_send_var.get()
        if not txt:
            return
        self.bridge_engine.send_rtt(txt.encode("utf-8"))
        self._rtt_send_var.set("")

    # ── Log Tab ──

    def _build_log_tab(self):
        f = ttk.Frame(self._nb)
        self._nb.add(f, text="  日志  ")

        bar = ttk.Frame(f)
        bar.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(bar, text="应用日志").pack(side=tk.LEFT)
        ttk.Button(bar, text="清屏", command=lambda: self._log_text.delete("1.0", tk.END)
                   ).pack(side=tk.RIGHT)

        self._log_text = scrolledtext.ScrolledText(f, wrap=tk.WORD, font=("Consolas", 9))
        self._log_text.pack(fill=tk.BOTH, expand=True, padx=4, pady=(0, 4))

    # ── Queue polling ──

    def _poll_queues(self):
        # Log
        try:
            while True:
                msg = self._log_q.get_nowait()
                ts = time.strftime("%H:%M:%S")
                self._log_text.insert(tk.END, f"[{ts}] {msg}\n")
                self._log_text.see(tk.END)

                # Update bridge status
                if "就绪" in msg or "已连接" in msg:
                    self._bridge_status.config(text="已连接", foreground="green")
                elif "断开" in msg or "失败" in msg or "停止" in msg:
                    if not self.bridge_engine.is_running:
                        self._bridge_status.config(text="未连接", foreground="gray")
                        self._bridge_btn.config(text="启动桥接")
        except queue.Empty:
            pass

        # Serial / TCP client data
        rx_batch = bytearray()
        try:
            while True:
                data = self._serial_rx_q.get_nowait()
                if self._serial_hex_var.get():
                    text = " ".join(f"{b:02X}" for b in data) + "\n"
                else:
                    text = data.decode("utf-8", errors="replace")
                self._term_insert(text, direction="rx")
                rx_batch.extend(data)
        except queue.Empty:
            pass
        if rx_batch:
            self._rx_feed_data(bytes(rx_batch))

        # Bridge serial data
        try:
            while True:
                data = self._bridge_serial_q.get_nowait()
                text = data.decode('utf-8', errors='replace')
                self._term_insert(text, direction="rx")
                rx_batch = bytearray(data)
                self._rx_feed_data(bytes(rx_batch))
        except queue.Empty:
            pass

        # RTT data
        try:
            while True:
                data = self._rtt_rx_q.get_nowait()
                text = data.decode('utf-8', errors='replace')
                self._term_insert(text, direction="rx")
        except queue.Empty:
            pass

        self.after(50, self._poll_queues)

    def _on_close(self):
        self._save_bridge_settings()
        # Save protocol settings
        if hasattr(self, '_plist_cycle_job') and self._plist_cycle_job is not None:
            self.after_cancel(self._plist_cycle_job)
            self._plist_cycle_job = None
        self._proto_save_config()
        self._stop_elaphurelink()
        self._stop_el_proxy()
        self.serial_engine.close()
        self.tcp_client_engine.close()
        self.bridge_engine.stop()
        self.destroy()


def main():
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()
