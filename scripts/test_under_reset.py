import mmap, os, struct, time

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
mem = mmap.mmap(fd, 4096, offset=0xE000A000)

def reg_r(off):
    return struct.unpack('<I', mem[off:off+4])[0]

def reg_w(off, val):
    mem[off:off+4] = struct.pack('<I', val)

PIN_SWCLK = 0
PIN_SWDIO = 1
PIN_NRST  = 2

def set_clk(val):
    mask = (~(1 << PIN_SWCLK)) & 0xFFFF
    reg_w(0x0010, (mask << 16) | ((1 if val else 0) << PIN_SWCLK))

def set_dio(val):
    mask = (~(1 << PIN_SWDIO)) & 0xFFFF
    reg_w(0x0010, (mask << 16) | ((1 if val else 0) << PIN_SWDIO))

def get_dio():
    return (reg_r(0x0068) >> PIN_SWDIO) & 1

def dio_out():
    reg_w(0x0284, reg_r(0x0284) | (1 << PIN_SWDIO))
    reg_w(0x0288, reg_r(0x0288) | (1 << PIN_SWDIO))

def dio_in():
    reg_w(0x0284, reg_r(0x0284) & ~(1 << PIN_SWDIO))
    reg_w(0x0288, reg_r(0x0288) & ~(1 << PIN_SWDIO))

def assert_reset(val):
    if val:
        mask = (~(1 << PIN_NRST)) & 0xFFFF
        reg_w(0x0010, (mask << 16) | (0 << PIN_NRST))
        reg_w(0x0284, reg_r(0x0284) | (1 << PIN_NRST))
        reg_w(0x0288, reg_r(0x0288) | (1 << PIN_NRST))
    else:
        reg_w(0x0284, reg_r(0x0284) & ~(1 << PIN_NRST))
        reg_w(0x0288, reg_r(0x0288) & ~(1 << PIN_NRST))

reg_w(0x0284, reg_r(0x0284) | (1 << PIN_SWCLK))
reg_w(0x0288, reg_r(0x0288) | (1 << PIN_SWCLK))
set_clk(1)
dio_out()
set_dio(1)

def delay():
    time.sleep(0.0001)

def write_bit(b):
    set_dio(b)
    set_clk(0); delay(); set_clk(1); delay()

def read_bit():
    set_clk(0); delay()
    b = get_dio()
    set_clk(1); delay()
    return b

def query():
    dio_out()
    for _ in range(60): write_bit(1)
    seq = 0xE79E
    for _ in range(16):
        write_bit(seq & 1)
        seq >>= 1
    for _ in range(60): write_bit(1)
    for _ in range(8): write_bit(0)

    req = 0xA5
    for _ in range(8):
        write_bit(req & 1)
        req >>= 1

    dio_in()
    set_clk(0); delay(); set_clk(1); delay()

    stream = [read_bit() for _ in range(40)]
    return ''.join(str(x) for x in stream)

print("RESET ASSERTED:")
assert_reset(True)
time.sleep(0.05)
print("Under reset:", query())

print("RESET RELEASED:")
assert_reset(False)
time.sleep(0.05)
print("After release:", query())
