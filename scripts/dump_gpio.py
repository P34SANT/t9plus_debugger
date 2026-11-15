import mmap, os, struct

fd = os.open('/dev/mem', os.O_RDWR)
m = mmap.mmap(fd, 4096, offset=0xE000A000)

for off in range(0, 0x300, 4):
    val = struct.unpack('<I', m[off:off+4])[0]
    if val != 0:
        print(f"0x{off:04X}: 0x{val:08X}")
