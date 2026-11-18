#!/usr/bin/env python3
"""Read STM32 trace registers over DAP without OpenOCD reconfiguring ITM."""
import struct
import argparse

import libusb_package
import usb.core
import usb.util

from swo_dap_test import dap_transfer, prepare_swd_target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-live-hardware", action="store_true",
                        help="Explicitly allow USB access and target debug writes")
    args = parser.parse_args()
    if not args.allow_live_hardware:
        parser.error("Live testing is paused after a Windows BSOD. "
                     "Explicit approval and --allow-live-hardware are required.")
    dev = usb.core.find(idVendor=0x0D28, idProduct=0x0204,
                        backend=libusb_package.get_libusb1_backend())
    if dev is None:
        raise RuntimeError("Attach Plink via USB/IP first")
    usb.util.claim_interface(dev, 0)
    try:
        prepare_swd_target(dev)
        # DP SELECT=0, AP CSW=32-bit word with address increment disabled.
        request = bytes([5, 0, 2, 8]) + struct.pack("<I", 0)
        request += bytes([1]) + struct.pack("<I", 0x23000042)
        reply = dap_transfer(dev, request)
        if reply[1:3] != bytes([2, 1]):
            raise RuntimeError(f"AP setup failed: {reply.hex(' ')}")
        for name, address in [
            ("DBGMCU_CR", 0xE0042004), ("TPI_CSPSR", 0xE0040004),
            ("TPI_SPPR", 0xE00400F0), ("TPI_ACPR", 0xE0040010),
            ("TPI_FFCR", 0xE0040304), ("ITM_TCR", 0xE0000E80),
            ("ITM_TER", 0xE0000E00), ("DEMCR", 0xE000EDFC),
            ("GPIOB_MODER", 0x40020400),
        ]:
            request = bytes([5, 0, 2, 5]) + struct.pack("<I", address) + bytes([15])
            reply = dap_transfer(dev, request)
            if len(reply) != 7 or reply[1:3] != bytes([2, 1]):
                raise RuntimeError(f"Read failed: {reply.hex(' ')}")
            print(f"{name:12s} 0x{address:08X} = 0x{struct.unpack_from('<I', reply, 3)[0]:08X}")
    finally:
        dap_transfer(dev, bytes([3]))
        usb.util.dispose_resources(dev)


if __name__ == "__main__":
    main()
