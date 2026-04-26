"""
elaphureLink Proxy — Pure Python implementation

Replaces elaphureLink.Wpf.exe + elaphureLinkProxy.dll.
Creates Windows shared memory and named events that Keil's
elaphureLinkRDDI.dll expects, then bridges CMSIS-DAP commands
to the DAP device over TCP (local bridge port).

Usage (standalone):
    python el_proxy.py --host 127.0.0.1 --port 3240

As library:
    from el_proxy import ElProxy
    proxy = ElProxy(log_callback=print)
    proxy.start("127.0.0.1", 3240)
    ...
    proxy.stop()
"""

import ctypes
import ctypes.wintypes as wt
import socket
import struct
import threading
import time
import traceback

# ─── elaphureLink IPC constants (must match ipc_common.hpp) ──────────────────

EL_SHARED_MEMORY_NAME = "elaphure.Memory"
EL_SHARED_MEMORY_SIZE = 4096 * 1001  # ~4 MB

EL_EVENT_PRODUCER_NAME = "elaphure.Event.Producer"
EL_EVENT_CONSUMER_NAME = "elaphure.Event.Consumer"

# Shared memory layout offsets
_PAGE_SIZE = 4096 * 500

# producer_page (offset 0)
OFF_PRODUCER_CMD_COUNT = 0
OFF_PRODUCER_DATA_LEN = 4
OFF_PRODUCER_DATA = 8

# consumer_page (offset _PAGE_SIZE)
OFF_CONSUMER_CMD_RESP = _PAGE_SIZE + 0
OFF_CONSUMER_DATA_LEN = _PAGE_SIZE + 4
OFF_CONSUMER_DATA = _PAGE_SIZE + 8

# info_page (offset 2*_PAGE_SIZE)
_INFO_BASE = 2 * _PAGE_SIZE
OFF_INFO_MAJOR = _INFO_BASE + 0
OFF_INFO_MINOR = _INFO_BASE + 4
OFF_INFO_REV = _INFO_BASE + 8
OFF_INFO_VERSION_STR = _INFO_BASE + 12
OFF_INFO_PROXY_READY = _INFO_BASE + 252  # 12 + 240
OFF_INFO_CAPABILITIES = _INFO_BASE + 256
OFF_INFO_PRODUCT_NAME = _INFO_BASE + 260
OFF_INFO_SERIAL_NUMBER = _INFO_BASE + 420  # 260 + 160
OFF_INFO_FIRMWARE_VER = _INFO_BASE + 580   # 420 + 160
OFF_INFO_DAP_BUF_SIZE = _INFO_BASE + 600   # 580 + 20

# elaphureLink protocol
EL_IDENTIFIER = 0x8A656C70
EL_CMD_HANDSHAKE = 0x00000000
EL_DAP_VERSION = 0x00000001

# DAP command IDs
ID_DAP_Info = 0x00
ID_DAP_Connect = 0x02
ID_DAP_Disconnect = 0x03
ID_DAP_TransferConfigure = 0x04
ID_DAP_Transfer = 0x05
ID_DAP_TransferBlock = 0x06
ID_DAP_WriteABORT = 0x08
ID_DAP_ResetTarget = 0x0A
ID_DAP_SWJ_Pins = 0x10
ID_DAP_SWJ_Clock = 0x11
ID_DAP_SWJ_Sequence = 0x12
ID_DAP_SWD_Configure = 0x13
ID_DAP_JTAG_Sequence = 0x14
ID_DAP_JTAG_Configure = 0x15
ID_DAP_ExecuteCommands = 0x7F

DAP_RES_OK = 1
DAP_RES_FAULT = 4

# Win32 constants
WAIT_OBJECT_0 = 0x00000000
WAIT_TIMEOUT = 0x00000102
INFINITE = 0xFFFFFFFF
EVENT_ALL_ACCESS = 0x1F0003
PAGE_READWRITE = 0x04
FILE_MAP_ALL_ACCESS = 0x000F001F

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ── Set proper argtypes/restype for 64-bit compatibility ──
kernel32.CreateEventW.argtypes = [wt.LPVOID, wt.BOOL, wt.BOOL, wt.LPCWSTR]
kernel32.CreateEventW.restype = wt.HANDLE

kernel32.SetEvent.argtypes = [wt.HANDLE]
kernel32.SetEvent.restype = wt.BOOL

kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.WaitForSingleObject.restype = wt.DWORD

kernel32.CreateFileMappingW.argtypes = [
    wt.HANDLE, wt.LPVOID, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPCWSTR
]
kernel32.CreateFileMappingW.restype = wt.HANDLE

kernel32.MapViewOfFile.argtypes = [
    wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t
]
kernel32.MapViewOfFile.restype = ctypes.c_void_p  # CRITICAL: 64-bit pointer

kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel32.UnmapViewOfFile.restype = wt.BOOL

kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL


def _create_event(name: str):
    """Create or open a named auto-reset event."""
    h = kernel32.CreateEventW(None, False, False, name)
    if not h:
        raise OSError(f"CreateEvent failed for {name}: {ctypes.get_last_error()}")
    return h


def _set_event(h):
    kernel32.SetEvent(h)


def _wait_event(h, timeout_ms=INFINITE):
    return kernel32.WaitForSingleObject(h, wt.DWORD(timeout_ms))


def _create_shared_memory(name: str, size: int):
    """Create or open a named file mapping (shared memory)."""
    INVALID_HANDLE = wt.HANDLE(-1)  # INVALID_HANDLE_VALUE (works on 64-bit)
    h = kernel32.CreateFileMappingW(
        INVALID_HANDLE,
        None,
        PAGE_READWRITE,
        0,
        size,
        name,
    )
    if not h:
        raise OSError(f"CreateFileMapping failed: {ctypes.get_last_error()}")
    return h


def _map_view(h, size):
    ptr = kernel32.MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, ctypes.c_size_t(size))
    if not ptr:
        raise OSError(f"MapViewOfFile failed: {ctypes.get_last_error()}")
    return ptr


def _unmap_view(ptr):
    if ptr:
        kernel32.UnmapViewOfFile(ctypes.c_void_p(ptr))


def _close_handle(h):
    if h:
        kernel32.CloseHandle(h)


class ElProxy:
    """Pure-Python replacement for elaphureLinkProxy.

    Creates the same Windows IPC objects that Keil's RDDI DLL expects,
    then proxies DAP commands to a TCP DAP server (our bridge or ESP32).
    """

    VERSION_STRING = "DAPLink-Tool-Proxy 1.4"

    def __init__(self, log_callback=None):
        self._log = log_callback or (lambda msg: None)
        self._running = False
        self._thread = None  # threading.Thread or None

        # IPC handles
        self._shm_handle = None
        self._shm_ptr = 0
        self._producer_event = None
        self._consumer_event = None

        # TCP socket to DAP device
        self._sock = None  # socket.socket or None

    @property
    def is_running(self):
        return self._running

    def start(self, host: str, port: int):
        """Start the proxy: create IPC, connect to DAP device, run loop."""
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(
            target=self._run, args=(host, port), daemon=True
        )
        self._thread.start()

    def stop(self):
        """Stop the proxy cleanly."""
        self._running = False
        # Wake up the producer wait so the loop can exit
        if self._producer_event:
            _set_event(self._producer_event)
        if self._consumer_event:
            _set_event(self._consumer_event)
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        # Wait for thread
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3)
        self._cleanup_ipc()

    # ─── Internal ─────────────────────────────────────────────────────────

    def _run(self, host: str, port: int):
        try:
            self._init_ipc()
        except Exception as e:
            self._log(f"Proxy IPC初始化错误: {e}")
            self._running = False
            return

        retry_count = 0
        max_retries = 30  # Maximum consecutive failures before giving up
        initial_retry_delay = 0.5  # Start with shorter delay for initial connection

        while self._running:
            try:
                self._connect_dap(host, port)
                self._do_handshake()
                self._get_device_info()
                self._mark_ready()
                self._log(f"elaphureLink Proxy 就绪 (DAP @ {host}:{port})")
                retry_count = 0  # Reset retry count on successful connection
                initial_retry_delay = 0.5  # Reset delay
                self._data_loop()
            except ConnectionRefusedError as e:
                # Connection refused - server not ready yet, retry quickly
                if not self._running:
                    break
                retry_count += 1
                self._log(f"Proxy 连接被拒绝 (服务器未就绪)")

                # Give up after too many consecutive failures
                if retry_count >= max_retries:
                    self._log(f"Proxy 连接失败次数过多 ({retry_count}次)，停止重试")
                    self._running = False
                    break

                # Use shorter delay for connection refused (server not ready)
                delay = min(initial_retry_delay * retry_count, 2.0)
                if retry_count <= 3 or retry_count % 5 == 0:
                    self._log(f"Proxy 等待桥接就绪... (尝试 {retry_count}/{max_retries})")
                for _ in range(int(delay * 10)):
                    if not self._running:
                        break
                    time.sleep(0.1)
                continue
            except Exception as e:
                if not self._running:
                    break
                retry_count += 1
                if retry_count <= 3 or retry_count % 5 == 0:
                    self._log(f"Proxy 连接错误: {e}")

                # Give up after too many consecutive failures
                if retry_count >= max_retries:
                    self._log(f"Proxy 连接失败次数过多 ({retry_count}次)，停止重试")
                    self._running = False
                    break
            finally:
                self._mark_not_ready()
                if self._sock:
                    try:
                        self._sock.close()
                    except Exception:
                        pass
                    self._sock = None

            if self._running and retry_count > 0:
                if retry_count <= 3 or retry_count % 5 == 0:
                    self._log(f"Proxy 将在 2 秒后重连... (尝试 {retry_count}/{max_retries})")
                for _ in range(20):  # 2s in 100ms chunks
                    if not self._running:
                        break
                    time.sleep(0.1)

        self._cleanup_ipc()
        self._running = False
        self._log("Proxy 已停止")

    def _init_ipc(self):
        """Create shared memory and named events."""
        self._shm_handle = _create_shared_memory(EL_SHARED_MEMORY_NAME, EL_SHARED_MEMORY_SIZE)
        self._shm_ptr = _map_view(self._shm_handle, EL_SHARED_MEMORY_SIZE)
        self._producer_event = _create_event(EL_EVENT_PRODUCER_NAME)
        self._consumer_event = _create_event(EL_EVENT_CONSUMER_NAME)

        # Zero out memory
        ctypes.memset(self._shm_ptr, 0, EL_SHARED_MEMORY_SIZE)

        # Write version info (must match elaphureRddi.dll expected version)
        self._write_u32(OFF_INFO_MAJOR, 1)
        self._write_u32(OFF_INFO_MINOR, 4)
        self._write_u32(OFF_INFO_REV, 0)
        self._write_str(OFF_INFO_VERSION_STR, "1.4.0.0", 240)

        self._log("IPC 已初始化 (共享内存 + 事件)")

    def _cleanup_ipc(self):
        if self._shm_ptr:
            # Mark not ready before cleanup
            try:
                self._write_u32(OFF_INFO_PROXY_READY, 0)
            except Exception:
                pass
            _unmap_view(self._shm_ptr)
            self._shm_ptr = 0
        _close_handle(self._shm_handle)
        self._shm_handle = None
        _close_handle(self._producer_event)
        self._producer_event = None
        _close_handle(self._consumer_event)
        self._consumer_event = None

    def _connect_dap(self, host: str, port: int):
        """TCP connect to the DAP server."""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock.settimeout(10)
        try:
            self._sock.connect((host, port))
            self._log(f"TCP 已连接: {host}:{port}")
        except ConnectionRefusedError as e:
            # Connection refused means the bridge server isn't listening yet
            raise ConnectionRefusedError(f"连接被拒绝 (服务器可能未就绪): {e}")
        except OSError as e:
            # Timeout, network-unreachable, reset, etc. — propagate as-is
            # so the caller uses the general-error retry path (longer delay)
            raise

    def _do_handshake(self):
        """Perform elaphureLink handshake with the DAP device."""
        req = struct.pack(">III", EL_IDENTIFIER, EL_CMD_HANDSHAKE, EL_DAP_VERSION)
        self._sock.sendall(req)
        resp = self._recv_exact(12)
        ident, cmd, ver = struct.unpack(">III", resp)
        if ident != EL_IDENTIFIER or cmd != EL_CMD_HANDSHAKE:
            raise RuntimeError("DAP 握手失败: 无效响应")
        self._log(f"DAP 握手成功 (版本: {ver})")

    def _get_device_info(self):
        """Query DAP_Info to populate the info page for RDDI."""
        # Product Name (ID=0x02)
        name = self._dap_info_query(0x02)
        self._write_str(OFF_INFO_PRODUCT_NAME, name, 160)

        # Serial Number (ID=0x03)
        sn = self._dap_info_query(0x03)
        self._write_str(OFF_INFO_SERIAL_NUMBER, sn, 160)

        # Firmware Version (ID=0x04)
        fw = self._dap_info_query(0x04)
        self._write_str(OFF_INFO_FIRMWARE_VER, fw, 20)

        # Capabilities (ID=0xF0)
        caps_raw = self._dap_info_query_raw(0xF0)
        if caps_raw:
            caps_val = caps_raw[0] if len(caps_raw) == 1 else (caps_raw[0] | (caps_raw[1] << 8))
            self._write_u32(OFF_INFO_CAPABILITIES, caps_val)

        self._log(f"设备: {name}, SN: {sn}, FW: {fw}")
        self._log(f"共享内存已填充设备信息，Keil RDDI应该可以读取")

    def _dap_info_query(self, info_id: int) -> str:
        """Send DAP_Info command and return the string result."""
        raw = self._dap_info_query_raw(info_id)
        return raw.decode("utf-8", errors="replace").rstrip("\x00") if raw else ""

    def _dap_info_query_raw(self, info_id: int) -> bytes:
        """Send DAP_Info and return raw bytes."""
        cmd = bytes([ID_DAP_Info, info_id])
        self._sock.sendall(cmd)
        resp = self._sock.recv(1500)
        if len(resp) < 2 or resp[0] != ID_DAP_Info:
            return b""
        length = resp[1]
        if length == 0:
            return b""
        return resp[2:2 + length]

    def _mark_ready(self):
        self._write_u32(OFF_INFO_PROXY_READY, 1)
        self._log("Proxy 已标记为就绪 (共享内存 PROXY_READY = 1)")

    def _mark_not_ready(self):
        if self._shm_ptr:
            try:
                self._write_u32(OFF_INFO_PROXY_READY, 0)
                # Signal consumer event to unblock any waiting RDDI call
                self._write_u32(OFF_CONSUMER_CMD_RESP, 0xFFFFFFFF)
                _set_event(self._consumer_event)
            except Exception:
                pass

    @staticmethod
    def _cmd_length(buf: bytes, offset: int) -> int:
        """Return the byte-length of one CMSIS-DAP command starting at offset.
        Returns -1 if the command is unknown or the buffer is too short.
        """
        if offset >= len(buf):
            return -1
        cmd = buf[offset]
        remaining = len(buf) - offset

        # Fixed-length commands
        FIXED = {
            0x00: 2,   # DAP_Info (cmd + id)
            0x01: 3,   # DAP_HostStatus (cmd + type + status)
            0x02: 2,   # DAP_Connect (cmd + port)
            0x03: 1,   # DAP_Disconnect
            0x04: 6,   # DAP_TransferConfigure (cmd + idle + 2×wait + 2×match)
            0x07: 1,   # DAP_TransferAbort
            0x08: 6,   # DAP_WriteABORT (cmd + dap_idx + 4×val)
            0x09: 3,   # DAP_Delay (cmd + 2×delay)
            0x0A: 1,   # DAP_ResetTarget
            0x10: 7,   # DAP_SWJ_Pins (cmd + out + sel + 4×wait)
            0x11: 5,   # DAP_SWJ_Clock (cmd + 4×clock)
            0x13: 2,   # DAP_SWD_Configure (cmd + config)
        }
        if cmd in FIXED:
            return FIXED[cmd] if remaining >= FIXED[cmd] else -1

        if cmd == 0x12:  # DAP_SWJ_Sequence
            if remaining < 2:
                return -1
            count = buf[offset + 1]
            if count == 0:
                count = 256
            data_bytes = (count + 7) // 8
            total = 2 + data_bytes
            return total if remaining >= total else -1

        if cmd == 0x05:  # DAP_Transfer
            if remaining < 3:
                return -1
            transfer_count = buf[offset + 2]
            pos = offset + 3
            for _ in range(transfer_count):
                if pos >= len(buf):
                    return -1
                req = buf[pos]; pos += 1
                if (req & 0x02) == 0:  # Write
                    pos += 4
            return pos - offset

        if cmd == 0x06:  # DAP_TransferBlock
            if remaining < 5:
                return -1
            count = buf[offset + 2] | (buf[offset + 3] << 8)
            req = buf[offset + 4]
            if (req & 0x02) == 0:  # Write
                return 5 + count * 4
            else:
                return 5

        if cmd == 0x14:  # DAP_JTAG_Sequence
            if remaining < 2:
                return -1
            seq_count = buf[offset + 1]
            pos = offset + 2
            for _ in range(seq_count):
                if pos >= len(buf):
                    return -1
                seq_info = buf[pos]; pos += 1
                tck = seq_info & 0x3F
                if tck == 0:
                    tck = 64
                pos += (tck + 7) // 8
            return pos - offset

        if cmd == 0x15:  # DAP_JTAG_Configure
            if remaining < 2:
                return -1
            count = buf[offset + 1]
            return 2 + count

        return -1  # Unknown

    def _split_and_execute_fallback(self, cmd_data: bytes) -> bytes:
        """Fallback: split ExecuteCommands batch into individual TCP sends.
        Only used for DAP devices that do NOT support native 0x7F handling.
        Normally, batches are forwarded directly via _data_loop.
        """
        num_cmds = cmd_data[1]
        inner = cmd_data[2:]
        responses = bytearray([ID_DAP_ExecuteCommands, num_cmds])

        offset = 0
        for i in range(num_cmds):
            cmd_len = self._cmd_length(inner, offset)
            if cmd_len <= 0:
                self._log(f"Proxy: 无法解析命令 #{i} offset={offset} byte=0x{inner[offset]:02x}")
                break

            single_cmd = inner[offset:offset + cmd_len]
            offset += cmd_len

            self._sock.sendall(single_cmd)
            self._sock.settimeout(45)
            resp = self._sock.recv(4096)
            if not resp:
                self._log("Proxy: DAP 返回空响应")
                break
            responses.extend(resp)

        return bytes(responses)

    def _data_loop(self):
        """Main proxy loop: wait for RDDI producer, forward to DAP, return response."""
        # DAP_Info response cache: {info_id -> response_bytes}
        # These values don't change during a session, so we cache after first query.
        _info_cache: dict = {}

        # Timing diagnostics
        _cmd_count = 0
        _session_start = time.monotonic()
        _CMD_NAME = {
            0x00: "Info", 0x01: "HostStatus", 0x02: "Connect", 0x03: "Disconnect",
            0x04: "XferConfig", 0x05: "Transfer", 0x06: "XferBlock", 0x07: "XferAbort",
            0x08: "WriteABORT", 0x09: "Delay", 0x0A: "ResetTarget",
            0x10: "SWJ_Pins", 0x11: "SWJ_Clock", 0x12: "SWJ_Seq",
            0x13: "SWD_Config", 0x7F: "ExecuteCmd",
        }

        while self._running:
            # Wait for RDDI to produce a command
            result = _wait_event(self._producer_event, 500)
            if result == WAIT_TIMEOUT:
                continue
            if not self._running:
                break

            # Read command from shared memory
            data_len = self._read_u32(OFF_PRODUCER_DATA_LEN)
            if data_len == 0 or data_len > _PAGE_SIZE - 8:
                continue

            cmd_data = self._read_bytes(OFF_PRODUCER_DATA, data_len)
            cmd_count = self._read_u32(OFF_PRODUCER_CMD_COUNT)

            # Reset consumer response
            self._write_u32(OFF_CONSUMER_CMD_RESP, 0xFFFFFFFF)

            # Optimization: cache DAP_Info responses (they don't change per session)
            if cmd_data[0] == ID_DAP_Info and len(cmd_data) >= 2:
                info_id = cmd_data[1]
                if info_id in _info_cache:
                    resp = _info_cache[info_id]
                    self._parse_response(resp, len(resp), cmd_count)
                    _set_event(self._consumer_event)
                    _cmd_count += 1
                    continue

            # Forward command directly to DAP device.
            # ExecuteCommands (0x7F) batches are forwarded as-is: the ESP32
            # natively handles them in el_process_execute_commands(), processing
            # all sub-commands in one TCP round trip instead of N separate ones.
            _t0 = time.monotonic()
            try:
                self._sock.sendall(cmd_data)
                self._sock.settimeout(45)
                resp = self._sock.recv(4096)
            except Exception as e:
                self._log(f"Proxy 通信错误: {e}")
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                _set_event(self._consumer_event)
                break
            _elapsed_ms = (time.monotonic() - _t0) * 1000
            _cmd_count += 1
            _total_ms = (time.monotonic() - _session_start) * 1000

            # Log slow commands (> 20ms) and periodic summary
            _cmd_name = _CMD_NAME.get(cmd_data[0], f"0x{cmd_data[0]:02X}")
            if _elapsed_ms > 20:
                self._log(f"[DAP #{_cmd_count:3d} +{_total_ms:.0f}ms] {_cmd_name} SLOW {_elapsed_ms:.1f}ms len={len(cmd_data)}")
            elif _cmd_count <= 30 or _cmd_count % 50 == 0:
                self._log(f"[DAP #{_cmd_count:3d} +{_total_ms:.0f}ms] {_cmd_name} {_elapsed_ms:.1f}ms")

            # Cache DAP_Info responses for future use
            if cmd_data[0] == ID_DAP_Info and len(cmd_data) >= 2 and resp:
                _info_cache[cmd_data[1]] = resp

            if not resp:
                self._log("Proxy: DAP 连接断开")
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                _set_event(self._consumer_event)
                break

            # Parse response and fill consumer page
            self._parse_response(resp, len(resp), cmd_count)

            # Notify RDDI
            _set_event(self._consumer_event)

    def _parse_response(self, resp: bytes, resp_len: int, cmd_count: int):
        """Parse DAP response and write results to consumer page.

        Handles ExecuteCommands (0x7F) wrapping multiple commands.
        Mirrors the C++ do_data_process() loop in protocol.cpp.
        """
        if resp_len < 1:
            self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
            return

        p = 0
        count = 1
        out_flag = False

        if resp[p] == ID_DAP_ExecuteCommands:
            if resp_len < 2:
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                return
            count = resp[p + 1]
            p += 2
            # Default result for ExecuteCmd batches: OK.
            # Individual Transfer/TransferBlock sub-responses will override this
            # if they encounter a fault. Setup-only batches (Connect, SWJ_*, etc.)
            # don't write their own status, so this ensures Keil sees success.
            self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK)

        for _ in range(count):
            if p >= resp_len or out_flag:
                break

            cmd_id = resp[p]

            if cmd_id == 0x00:               # DAP_Info response: [cmd, len, data...]
                if p + 1 < resp_len:
                    p += 2 + resp[p + 1]     # skip cmd + len + data bytes
                else:
                    p += 1
                continue

            if cmd_id == 0x01:               # DAP_HostStatus: [cmd, status]
                p += 2

            elif cmd_id == ID_DAP_Connect:
                p += 2

            elif cmd_id == ID_DAP_Disconnect:
                p += 2

            elif cmd_id == 0x07:             # DAP_TransferAbort: [cmd, status]
                p += 2

            elif cmd_id == 0x09:             # DAP_Delay: [cmd, status]
                p += 2

            elif cmd_id == ID_DAP_TransferConfigure:
                p += 2
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK)

            elif cmd_id == ID_DAP_Transfer:
                if p + 3 > resp_len:
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    return
                p += 1
                transfer_count = resp[p]; p += 1
                status = resp[p]; p += 1

                if transfer_count != cmd_count:
                    out_flag = True
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    break

                self._write_u32(OFF_CONSUMER_CMD_RESP, status)
                if status != DAP_RES_OK:
                    out_flag = True
                    break

                remain = resp_len - p
                if remain > 0:
                    self._write_u32(OFF_CONSUMER_DATA_LEN, remain)
                    self._write_bytes(OFF_CONSUMER_DATA, resp[p:p + remain])
                break  # Transfer consumes remaining data

            elif cmd_id == ID_DAP_TransferBlock:
                if p + 4 > resp_len:
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    return
                p += 1
                tc_lo = resp[p]; p += 1
                tc_hi = resp[p]; p += 1
                transfer_count = (tc_hi << 8) | tc_lo
                status = resp[p]; p += 1

                if transfer_count != cmd_count:
                    out_flag = True
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    break

                self._write_u32(OFF_CONSUMER_CMD_RESP, status)
                if status != DAP_RES_OK and status != DAP_RES_FAULT:
                    out_flag = True
                    break

                remain = resp_len - p
                if remain > 0:
                    self._write_u32(OFF_CONSUMER_DATA_LEN, remain)
                    self._write_bytes(OFF_CONSUMER_DATA, resp[p:p + remain])
                break  # TransferBlock consumes remaining data

            elif cmd_id == ID_DAP_WriteABORT:
                if p + 1 < resp_len and resp[p + 1] != 0:
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    out_flag = True
                else:
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK)
                p += 2

            elif cmd_id == ID_DAP_ResetTarget:
                p += 3

            elif cmd_id == ID_DAP_SWJ_Pins:
                if p + 1 < resp_len:
                    self._write_u32(OFF_CONSUMER_DATA_LEN, 1)
                    self._write_bytes(OFF_CONSUMER_DATA, bytes([resp[p + 1]]))
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK)
                p += 2

            elif cmd_id == ID_DAP_JTAG_Sequence:
                if p + 1 < resp_len and resp[p + 1] != 0:
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_FAULT)
                    out_flag = True
                    break
                p += 2
                remain = resp_len - p
                if remain > 0 and remain == cmd_count:
                    self._write_u32(OFF_CONSUMER_DATA_LEN, remain)
                    self._write_bytes(OFF_CONSUMER_DATA, resp[p:p + remain])
                self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK)
                break

            elif cmd_id == ID_DAP_JTAG_Configure:
                if p + 1 < resp_len:
                    status_byte = resp[p + 1]
                    self._write_u32(OFF_CONSUMER_CMD_RESP, DAP_RES_OK if status_byte == 0 else 0xFF)
                p += 2

            elif cmd_id == ID_DAP_SWJ_Clock:
                p += 2

            elif cmd_id == ID_DAP_SWJ_Sequence:
                # Don't check status byte — non-zero status is reported by some firmware
                # versions even on success; treating it as FAULT causes spurious failures.
                p += 2

            elif cmd_id == ID_DAP_SWD_Configure:
                p += 2

            elif 0x80 <= cmd_id <= 0x8A:   # DAP_SWO_* — ESP32 unsupported, returns 1-byte [cmd]
                p += 1

            elif cmd_id == 0x16:           # DAP_JTAG_IDCODE — ESP32 returns 1-byte (unsupported)
                p += 1

            else:
                # Unknown command: ESP32 firmware returns 1-byte [cmd_id] for any unrecognised cmd.
                # Advance by 1 and continue rather than breaking mid-ExecuteCmd batch.
                self._log(f"Proxy: 未知 DAP 响应 0x{cmd_id:02X} (跳过1字节)")
                p += 1

    # ─── Shared memory helpers ────────────────────────────────────────────

    def _read_u32(self, offset: int) -> int:
        val = ctypes.c_uint32.from_address(self._shm_ptr + offset).value
        return val

    def _write_u32(self, offset: int, value: int):
        ctypes.c_uint32.from_address(self._shm_ptr + offset).value = value & 0xFFFFFFFF

    def _read_bytes(self, offset: int, length: int) -> bytes:
        return (ctypes.c_char * length).from_address(self._shm_ptr + offset).raw

    def _write_bytes(self, offset: int, data: bytes):
        ctypes.memmove(self._shm_ptr + offset, data, len(data))

    def _write_str(self, offset: int, s: str, max_len: int):
        b = s.encode("utf-8")[:max_len - 1] + b"\x00"
        self._write_bytes(offset, b)

    def _recv_exact(self, n: int) -> bytes:
        """Receive exactly n bytes from socket."""
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("连接断开")
            buf.extend(chunk)
        return bytes(buf)


# ─── Standalone entry point ──────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="elaphureLink Proxy (Python)")
    parser.add_argument("--host", default="127.0.0.1", help="DAP TCP host")
    parser.add_argument("--port", type=int, default=3240, help="DAP TCP port")
    args = parser.parse_args()

    proxy = ElProxy(log_callback=lambda m: print(f"[Proxy] {m}"))

    print(f"启动 elaphureLink Proxy -> {args.host}:{args.port}")
    print("Keil 中选择 elaphureLink 调试器即可使用")
    print("按 Ctrl+C 退出")

    proxy.start(args.host, args.port)

    try:
        while proxy.is_running:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n正在退出...")

    proxy.stop()
