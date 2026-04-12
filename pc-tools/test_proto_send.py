"""
测试程序：按协议格式通过COM5每10ms发送一包数据
帧头: AA 55 (2B)
长度: 0C (1B, 包含所有字段)
命令: 01 (1B)
数据: int16 LE, ±500正弦波
数据1: float LE, ±450.5正弦波, 相位差90度
校验: CRC16/MODBUS (2B)
"""

import serial
import struct
import math
import time

PORT = "COM5"
BAUD = 1152000
INTERVAL = 0.005  # 10ms
FREQ = 10.0       # 1Hz 正弦波频率


def crc16_modbus(data: bytes) -> bytes:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return struct.pack('<H', crc)


def build_packet(t: float) -> bytes:
    header = b'\xAA\x55'
    length = struct.pack('B', 0x0C)  # 12 = all fields including self
    cmd = b'\x01'
    # 数据: int16 LE, ±500正弦波
    val1 = int(500.0 * math.sin(2 * math.pi * FREQ * t))
    val1 = max(-32768, min(32767, val1))
    data1 = struct.pack('<h', val1)
    # 数据1: float LE, ±450.5正弦波, 相位差90度
    val2 = 450.5 * math.sin(2 * math.pi * FREQ * t + math.pi / 2)
    data2 = struct.pack('<f', val2)
    # CRC16/MODBUS over all fields except CRC itself
    payload = header + length + cmd + data1 + data2
    crc = crc16_modbus(payload)
    return payload + crc


def main():
    print(f"打开 {PORT} @ {BAUD}bps")
    ser = serial.Serial(PORT, BAUD, timeout=1)
    print(f"开始发送，间隔 {INTERVAL*1000:.0f}ms，频率 {FREQ}Hz")
    print("按 Ctrl+C 停止")

    t0 = time.time()
    count = 0
    try:
        while True:
            t = time.time() - t0
            pkt = build_packet(t)
            ser.write(pkt)
            count += 1
            if count % 100 == 0:
                hex_str = " ".join(f"{b:02X}" for b in pkt)
                print(f"[{count}] t={t:.2f}s  {hex_str}")
            # Precise timing
            next_time = t0 + count * INTERVAL
            sleep_time = next_time - time.time()
            if sleep_time > 0:
                time.sleep(sleep_time)
    except KeyboardInterrupt:
        print(f"\n已停止，共发送 {count} 包")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
