#!/usr/bin/env python3
"""Quick DAP protocol probe: test SWO_STATUS (1-byte cmd) and INFO (2-byte cmd)."""
import usb.core, usb.util, struct, sys, time
import usb.backend.libusb1
import libusb_package

DAP_VID, DAP_PID = 0x0D28, 0x0204

def dap_transfer(dev, cmd: bytes, timeout=2000) -> bytes:
    dev.write(0x01, cmd, timeout)
    return dev.read(0x81, 512, timeout)

dev = usb.core.find(idVendor=DAP_VID, idProduct=DAP_PID,
                    backend=libusb_package.get_libusb1_backend())
if dev is None:
    print("[-] no device"); sys.exit(1)
try:
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
except Exception:
    pass
dev.set_configuration()
print(f"[+] device: {dev.product!r}")

tests = [
    ("SWO_STATUS        ", bytes([0x1B])),
    ("SWO_STATUS again  ", bytes([0x1B])),
    ("INFO CAPABILITIES ", bytes([0x04, 0xF0])),
    ("INFO FW_VERSION   ", bytes([0x04, 0x09])),
    ("INFO SWO_BUF_SIZE ", bytes([0x04, 0xFD])),
    ("SWO_MODE UART     ", bytes([0x18, 0x01])),
    ("SWO_BAUDRATE 2M   ", bytes([0x19]) + struct.pack('<I', 2_000_000)),
    ("SWO_CONTROL START ", bytes([0x1A, 0x01])),
    ("SWO_DATA 64       ", bytes([0x1C]) + struct.pack('<H', 64)),
    ("SWO_CONTROL STOP  ", bytes([0x1A, 0x00])),
]
for name, cmd in tests:
    try:
        r = dap_transfer(dev, cmd)
        print(f"{name} -> len={len(r):3d} {list(r)[:12]}")
    except Exception as e:
        print(f"{name} -> ERROR {e}")
    time.sleep(0.05)
