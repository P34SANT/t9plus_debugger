#!/usr/bin/env python3
"""Isolate the INFO 1-byte response issue."""
import usb.core, struct, sys, time
import libusb_package

def dap(dev, cmd: bytes, timeout=2000):
    dev.write(0x01, cmd, timeout)
    return dev.read(0x81, 512, timeout)

dev = usb.core.find(idVendor=0x0D28, idProduct=0x0204,
                    backend=libusb_package.get_libusb1_backend())
dev.set_configuration()

print("host_status  [01,00]     ->", list(dap(dev, bytes([0x01, 0x00]))))
print("info caps    [04,F0]     ->", list(dap(dev, bytes([0x04, 0xF0]))))
print("info caps pad[04,F0,00]  ->", list(dap(dev, bytes([0x04, 0xF0, 0x00]))))
print("info caps pad[04,F0,00*8]->", list(dap(dev, bytes([0x04, 0xF0] + [0]*8))))
print("info pktcount[04,FE]     ->", list(dap(dev, bytes([0x04, 0xFE]))))
print("info pktsize [04,FF]     ->", list(dap(dev, bytes([0x04, 0xFF]))))
print("connect swd  [00,01]     ->", list(dap(dev, bytes([0x00, 0x01]))))
print("swo status   [1B]        ->", list(dap(dev, bytes([0x1B]))))
