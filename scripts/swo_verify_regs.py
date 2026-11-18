#!/usr/bin/env python3
"""Verify SWO engine registers on the T9+ board via /dev/mem."""
import mmap, struct, sys

SWO_BASE = 0x43C20000
PAGE = 4096

fd = os_open = __import__('os').open('/dev/mem', __import__('os').O_RDWR | __import__('os').O_SYNC)
mm = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=SWO_BASE & ~0xFFF)

def rd(off):
    return struct.unpack('<I', mm[off:off+4])[0]

def wr(off, val):
    mm[off:off+4] = struct.pack('<I', val)

id_reg   = rd(0x18)
ctrl     = rd(0x00)
clkdiv   = rd(0x04)
status   = rd(0x10)
count    = rd(0x14)
total    = rd(0x1C)

print(f"CORE_ID   @0x18 = 0x{id_reg:08X} (expect 0x53574F01)")
print(f"CTRL      @0x00 = 0x{ctrl:08X} (enable={ctrl&1}, ch_sel={(ctrl>>2)&1}, invert={(ctrl>>3)&1})")
print(f"CLK_DIV   @0x04 = {clkdiv} (default 50 = 2.0 Mbps @ 100 MHz)")
print(f"STATUS    @0x10 = 0x{status:08X}")
print(f"FIFO_CNT  @0x14 = {count}")
print(f"TOTAL_RX  @0x1C = {total}")

if id_reg != 0x53574F01:
    print("ERROR: SWO engine not detected!")
    sys.exit(1)

# Quick sanity: enable capture for Pod 1 (J3 Pin 6), then read STATUS + pop a byte
wr(0x00, 0x00000001)  # enable=1, channel=0 (Pod1 J3 T10)
import time
time.sleep(0.5)
status2 = rd(0x10)
count2  = rd(0x14)
print(f"\nAfter enable: STATUS=0x{status2:08X} FIFO_CNT={count2} TOTAL_RX={rd(0x1C)}")
data = rd(0x08)
print(f"Pop byte  : 0x{data:08X} (valid={(data>>8)&1}, byte=0x{data&0xFF:02X})")
# Disable again
wr(0x00, 0x00000000)
print("[+] SWO engine register interface verified OK")
