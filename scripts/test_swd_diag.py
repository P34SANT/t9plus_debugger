#!/usr/bin/env python3
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

# Pin control
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

# Release reset
reg_w(0x0284, reg_r(0x0284) & ~(1 << PIN_NRST))
reg_w(0x0288, reg_r(0x0288) & ~(1 << PIN_NRST))

# CLK output
reg_w(0x0284, reg_r(0x0284) | (1 << PIN_SWCLK))
reg_w(0x0288, reg_r(0x0288) | (1 << PIN_SWCLK))
set_clk(1)
dio_out()
set_dio(1)
time.sleep(0.05)

def delay():
    for _ in range(50):
        pass

def clk_cycle():
    set_clk(0)
    delay()
    set_clk(1)
    delay()

def write_bit(b):
    set_dio(b)
    set_clk(0)
    delay()
    set_clk(1)
    delay()

def read_bit():
    set_clk(0)
    delay()
    b = get_dio()
    set_clk(1)
    delay()
    return b

def swd_init_seq():
    dio_out()
    # 60 cycles high
    for _ in range(60):
        write_bit(1)
    # JTAG to SWD: 0xE79E (0b0111100111100111 LSB first)
    seq = 0xE79E
    for _ in range(16):
        write_bit(seq & 1)
        seq >>= 1
    # 60 cycles high
    for _ in range(60):
        write_bit(1)
    # 8 idle zeroes
    for _ in range(8):
        write_bit(0)

swd_init_seq()

# Read DPIDR: request 0xA5
req = 0xA5
for _ in range(8):
    write_bit(req & 1)
    req >>= 1

# Turnaround
dio_in()
clk_cycle()

# Read ACK
ack = 0
for i in range(3):
    ack |= (read_bit() << i)

print("SWD ACK:", ack)
if ack == 1:
    val = 0
    parity = 0
    for i in range(32):
        b = read_bit()
        val |= (b << i)
        parity ^= b
    p_bit = read_bit()
    print("SUCCESS! DPIDR = 0x%08X, Parity: %s" % (val, parity == p_bit))
else:
    print("Target did not ACK with 1 (got %d). Target line status: dio=%d" % (ack, get_dio()))

clk_cycle()
dio_out()
for _ in range(8):
    write_bit(0)
