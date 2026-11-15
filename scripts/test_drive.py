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

def set_pin(pin, val):
    mask = (~(1 << pin)) & 0xFFFF
    reg_w(0x0010, (mask << 16) | ((1 if val else 0) << pin))

def pin_out(pin):
    reg_w(0x0284, reg_r(0x0284) | (1 << pin))
    reg_w(0x0288, reg_r(0x0288) | (1 << pin))

def pin_in(pin):
    reg_w(0x0284, reg_r(0x0284) & ~(1 << pin))
    reg_w(0x0288, reg_r(0x0288) & ~(1 << pin))

def read_pin(pin):
    return (reg_r(0x0068) >> pin) & 1

print("Testing loopback / drive capabilities on EMIO pins:")
pin_out(PIN_SWCLK)
set_pin(PIN_SWCLK, 0)
time.sleep(0.01)
print("SWCLK drove 0 -> read:", read_pin(PIN_SWCLK))
set_pin(PIN_SWCLK, 1)
time.sleep(0.01)
print("SWCLK drove 1 -> read:", read_pin(PIN_SWCLK))

pin_out(PIN_SWDIO)
set_pin(PIN_SWDIO, 0)
time.sleep(0.01)
print("SWDIO drove 0 -> read:", read_pin(PIN_SWDIO))
set_pin(PIN_SWDIO, 1)
time.sleep(0.01)
print("SWDIO drove 1 -> read:", read_pin(PIN_SWDIO))

pin_in(PIN_SWDIO)
time.sleep(0.01)
print("SWDIO set to input (tristate) -> read:", read_pin(PIN_SWDIO))
