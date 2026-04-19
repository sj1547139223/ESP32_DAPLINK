#!/usr/bin/env python3
"""Test CMSIS-DAP via WinUSB"""

import usb.core
import usb.util
import sys

def find_daplink():
    """Find DAPLink device"""
    dev = usb.core.find(idVendor=0x0D28, idProduct=0x0204)
    return dev

def main():
    print("Searching for DAPLink device (VID=0x0D28, PID=0x0204)...")
    dev = find_daplink()

    if dev is None:
        print("ERROR: DAPLink device not found!")
        print("\nAvailable USB devices:")
        for d in usb.core.find(find_all=True):
            print(f"  VID={d.idVendor:04x} PID={d.idProduct:04x}")
        return 1

    print(f"Found device!")
    print(f"  Manufacturer: {usb.util.get_string(dev, dev.iManufacturer)}")
    print(f"  Product: {usb.util.get_string(dev, dev.iProduct)}")
    print(f"  Serial: {usb.util.get_string(dev, dev.iSerialNumber)}")
    
    # Print configuration
    cfg = dev.get_active_configuration()
    print(f"\nConfiguration:")
    for intf in cfg:
        print(f"  Interface {intf.bInterfaceNumber}: Class={intf.bInterfaceClass}")
        for ep in intf:
            print(f"    Endpoint 0x{ep.bEndpointAddress:02x}: Type={ep.bmAttributes}")

    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)
