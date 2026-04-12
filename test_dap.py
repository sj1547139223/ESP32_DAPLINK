#!/usr/bin/env python3
"""Test CMSIS-DAP HID communication"""

import hid
import sys

def find_daplink():
    """Find DAPLink device"""
    devices = hid.enumerate(0x0D28, 0x0204)
    for dev in devices:
        if dev['interface_number'] == 0:
            return dev
    return None

def test_dap_info(device):
    """Test DAP_Info commands"""
    info_ids = {
        0x01: "Vendor",
        0x02: "Product",
        0x03: "Serial",
        0x04: "FW Version",
        0xF0: "Capabilities",
        0xFF: "Packet Size"
    }

    print("Testing DAP_Info commands:")
    for info_id, name in info_ids.items():
        # Send DAP_Info command
        cmd = [0x00, info_id] + [0] * 62
        device.write(cmd)

        # Read response
        response = device.read(64, timeout_ms=1000)
        if response and response[0] == 0x00:
            length = response[1]
            if length > 0:
                if info_id in [0xF0, 0xFF]:
                    value = ' '.join(f'{b:02x}' for b in response[2:2+length])
                else:
                    value = bytes(response[2:2+length]).decode('utf-8', errors='ignore')
                print(f"  {name}: {value}")
            else:
                print(f"  {name}: (empty)")
        else:
            print(f"  {name}: No response")

def main():
    print("Searching for DAPLink device (VID=0x0D28, PID=0x0204)...")
    dev_info = find_daplink()

    if not dev_info:
        print("ERROR: DAPLink device not found!")
        print("\nAvailable HID devices:")
        for d in hid.enumerate():
            print(f"  VID={d['vendor_id']:04x} PID={d['product_id']:04x} - {d['product_string']}")
        return 1

    print(f"Found: {dev_info['product_string']}")
    print(f"Path: {dev_info['path']}")

    try:
        device = hid.device()
        device.open_path(dev_info['path'])
        print("\nDevice opened successfully!")

        test_dap_info(device)

        device.close()
        print("\nTest completed successfully!")
        return 0

    except Exception as e:
        print(f"\nERROR: {e}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
