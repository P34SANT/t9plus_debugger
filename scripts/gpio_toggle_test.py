#!/usr/bin/env python3
"""Live GPIO register sanity: read all banks, toggle EMIO bit3, re-read."""
import mmap, struct, os, time

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
mm = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xE000A000)

def rd(off):
    return struct.unpack('<I', mm[off:off+4])[0]

def wr(off, val):
    mm[off:off+4] = struct.pack('<I', val)

print("=== Bank 2 (EMIO 54..69) ===")
print(f"DIRM_2    (0x284) = 0x{rd(0x284):08X}")
print(f"OEN_2     (0x288) = 0x{rd(0x288):08X}")
print(f"DATA_2_RO (0x068) = 0x{rd(0x068):08X}")
print(f"DATA_2    (0x048) = 0x{rd(0x048):08X}")
print(f"MASK_2LSW (0x010) = 0x{rd(0x010):08X}")
print("=== Bank 1 (MIO 32..47) ===")
print(f"DIRM_1    (0x244) = 0x{rd(0x244):08X}")

# Toggle test: set EMIO bit 3 direction out, write 1, read back, then restore
wr(0x284, rd(0x284) | (1 << 3))
wr(0x288, rd(0x288) | (1 << 3))
wr(0x010, (0xFFF7 << 16) | (1 << 3))   # MASK_DATA_2_LSW: only bit3 written = 1
time.sleep(0.05)
print(f"\nAfter drive bit3=1: DATA_2_RO=0x{rd(0x068):08X} bit3={(rd(0x068)>>3)&1}")
wr(0x010, (0xFFF7 << 16) | (0 << 3))   # bit3 = 0
time.sleep(0.05)
print(f"After drive bit3=0: DATA_2_RO=0x{rd(0x068):08X} bit3={(rd(0x068)>>3)&1}")
# Restore input mode
wr(0x288, rd(0x288) & ~(1 << 3))
wr(0x284, rd(0x284) & ~(1 << 3))
print(f"Restored input: DIRM_2=0x{rd(0x284):08X} OEN_2=0x{rd(0x288):08X} DATA_2_RO=0x{rd(0x068):08X} bit3={(rd(0x068)>>3)&1}")
