import mmap, os, struct
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, offset=0xE000A000)
for reg, name in [(0x0048, "DATA_2"), (0x0068, "DATA_2_RO"), (0x0284, "DIRM_2"), (0x0288, "OEN_2")]:
    val = struct.unpack('<I', m[reg:reg+4])[0]
    print(f"{name} (0x{reg:04X}): 0x{val:08X}")
