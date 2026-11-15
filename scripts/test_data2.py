import mmap, os, struct, time

fd = os.open('/dev/mem', os.O_RDWR)
m = mmap.mmap(fd, 4096, offset=0xE000A000)

def read_ro():
    return struct.unpack('<I', m[0x0068:0x006C])[0]

def read_data2():
    return struct.unpack('<I', m[0x0048:0x004C])[0]

def write_data2(val):
    m[0x0048:0x004C] = struct.pack('<I', val)

# Configure pin 0, 1 as outputs:
# DIRM_2 (0x0284), OEN_2 (0x0288)
dirm = struct.unpack('<I', m[0x0284:0x0288])[0] | 3
oen = struct.unpack('<I', m[0x0288:0x028C])[0] | 3
m[0x0284:0x0288] = struct.pack('<I', dirm)
m[0x0288:0x028C] = struct.pack('<I', oen)

print("Current DATA_2:", hex(read_data2()), "DATA_2_RO:", hex(read_ro()))

# Clear bit 0 (SWCLK = 0)
write_data2(read_data2() & ~1)
print("After SWCLK=0 -> DATA_2:", hex(read_data2()), "DATA_2_RO:", hex(read_ro()))

# Set bit 0 (SWCLK = 1)
write_data2(read_data2() | 1)
print("After SWCLK=1 -> DATA_2:", hex(read_data2()), "DATA_2_RO:", hex(read_ro()))

# Clear bit 1 (SWDIO = 0)
write_data2(read_data2() & ~2)
print("After SWDIO=0 -> DATA_2:", hex(read_data2()), "DATA_2_RO:", hex(read_ro()))

# Set bit 1 (SWDIO = 1)
write_data2(read_data2() | 2)
print("After SWDIO=1 -> DATA_2:", hex(read_data2()), "DATA_2_RO:", hex(read_ro()))
