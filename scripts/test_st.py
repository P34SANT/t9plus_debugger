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

def set_pin_dir(pin, is_out):
    dirm = reg_r(0x0284)
    oen = reg_r(0x0288)
    if is_out:
        reg_w(0x0284, dirm | (1 << pin))
        reg_w(0x0288, oen | (1 << pin))
    else:
        reg_w(0x0284, dirm & ~(1 << pin))
        reg_w(0x0288, oen & ~(1 << pin))

def read_pin(pin):
    return (reg_r(0x0068) >> pin) & 1

# Hardware SWD MUX: clear bit 15 of 0x43C00000 to ensure EMIO has pin control
hw_fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
hw_mem = mmap.mmap(hw_fd, 4096, offset=0x43C00000)
hw_mem[0:4] = struct.pack('<I', 0)

set_pin_dir(PIN_NRST, False)
set_pin_dir(PIN_SWCLK, True)
set_pin_dir(PIN_SWDIO, True)
set_pin(PIN_SWCLK, 1)
set_pin(PIN_SWDIO, 1)
time.sleep(0.01)

print(f"DIRM_2: 0x{reg_r(0x0284):08X}, OEN_2: 0x{reg_r(0x0288):08X}, DATA_2: 0x{reg_r(0x0048):08X}, DATA_2_RO: 0x{reg_r(0x0068):08X}")
print(f"SWCLK={read_pin(PIN_SWCLK)}, SWDIO={read_pin(PIN_SWDIO)}, nRST={read_pin(PIN_NRST)}")

def clk_delay():
    # short delay ~1 us
    for _ in range(100): pass

def write_bit(bit):
    set_pin(PIN_SWDIO, bit)
    set_pin(PIN_SWCLK, 0)
    clk_delay()
    set_pin(PIN_SWCLK, 1)
    clk_delay()

def read_bit():
    set_pin(PIN_SWCLK, 0)
    clk_delay()
    b = read_pin(PIN_SWDIO)
    set_pin(PIN_SWCLK, 1)
    clk_delay()
    return b

def trn():
    set_pin(PIN_SWCLK, 0)
    clk_delay()
    set_pin(PIN_SWCLK, 1)
    clk_delay()

print("\n--- Sending Line Reset & JTAG-to-SWD ---")
set_pin_dir(PIN_SWDIO, True)
for _ in range(60): write_bit(1)
seq = 0xE79E
for _ in range(16):
    write_bit(seq & 1)
    seq >>= 1
for _ in range(60): write_bit(1)
for _ in range(8): write_bit(0)

print("--- Reading DPIDR ---")
# DPIDR req: 0xA5 (LSB first: 1, 0, 1, 0, 0, 1, 0, 1 -> 0xA5)
# APnDP=0, RnW=1, Addr=00
req = 0xA5
for _ in range(8):
    write_bit(req & 1)
    req >>= 1

set_pin_dir(PIN_SWDIO, False) # Host turnaround: release SWDIO to input
trn()

# Sample 3-bit ACK
ack = 0
ack |= (read_bit() << 0)
ack |= (read_bit() << 1)
ack |= (read_bit() << 2)

val = 0
par = 0
for i in range(32):
    b = read_bit()
    val |= (b << i)
    par ^= b

par_bit = read_bit()
trn()
set_pin_dir(PIN_SWDIO, True)
set_pin(PIN_SWDIO, 1)
for _ in range(8): write_bit(0)

print(f"ACK={ack} (1=OK, 2=WAIT, 4=FAULT)")
print(f"DPIDR=0x{val:08X} (Expected: 0x2BA01477)")
print(f"Parity: {'OK' if par == par_bit else 'FAIL'} (calculated {par}, received {par_bit})")
