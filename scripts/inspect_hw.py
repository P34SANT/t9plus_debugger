import mmap, os, struct, time

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
hw = mmap.mmap(fd, 4096, offset=0x43C00000)
gpio = mmap.mmap(fd, 4096, offset=0xE000A000)

def hw_r(off):
    return struct.unpack('<I', hw[off:off+4])[0]

def hw_w(off, val):
    hw[off:off+4] = struct.pack('<I', val)

def gpio_r(off):
    return struct.unpack('<I', gpio[off:off+4])[0]

def gpio_w(off, val):
    gpio[off:off+4] = struct.pack('<I', val)

print("--- HW SWD Registers (0x43C00000) ---")
for off in range(0, 0x20, 4):
    print(f"HW 0x{off:02X}: 0x{hw_r(off):08X}")

print("--- GPIO Registers (0xE000A000) ---")
print(f"DATA_2 (0x48):    0x{gpio_r(0x48):08X}")
print(f"DATA_2_RO (0x68): 0x{gpio_r(0x68):08X}")
print(f"DIRM_2 (0x284):   0x{gpio_r(0x284):08X}")
print(f"OEN_2 (0x288):    0x{gpio_r(0x288):08X}")

# Check pin states
ro = gpio_r(0x68)
print(f"Pin SWCLK (bit 0): {(ro >> 0) & 1}")
print(f"Pin SWDIO (bit 1): {(ro >> 1) & 1}")
print(f"Pin nRST  (bit 2): {(ro >> 2) & 1}")
