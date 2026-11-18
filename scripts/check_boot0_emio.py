#!/usr/bin/env python3
"""Check EMIO bank2 state for BOOT0/T10 (EMIO 3 = GPIO 57)."""
import ctypes
import mmap
import os

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
mm = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xE000A000)

def rd(off):
    # Zynq PS peripherals require aligned 32-bit accesses, not mmap byte slices.
    return ctypes.c_uint32.from_buffer(mm, off).value

dirm2 = rd(0x284)
oen2 = rd(0x288)
data2 = rd(0x068)   # Bank 2 input data (DATA_2_RO workaround)
b = 3               # EMIO 3 = GPIO 57 = BOOT0/T10
print(f'DIRM_2    = 0x{dirm2:08X}  bit3 (dir, 1=out) = {(dirm2 >> b) & 1}')
print(f'OEN_2     = 0x{oen2:08X}  bit3 (oen, 1=drive) = {(oen2 >> b) & 1}')
print(f'DATA_2_RO = 0x{data2:08X}  bit3 (pin level)    = {(data2 >> b) & 1}')
