import mmap, os, struct, time

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
mem = mmap.mmap(fd, 4096, offset=0x43C00000)

SWD_REG_CTRL    = 0x00
SWD_REG_CLK_DIV = 0x04
SWD_REG_WDATA   = 0x08
SWD_REG_RDATA   = 0x0C
SWD_REG_STATUS  = 0x10
SWD_REG_RAW_LEN = 0x14
SWD_REG_ID      = 0x18

def reg_r(off):
    return struct.unpack('<I', mem[off:off+4])[0]

def reg_w(off, val):
    mem[off:off+4] = struct.pack('<I', val)

print("AXI SWD Engine ID:", hex(reg_r(SWD_REG_ID)))

def swd_raw_write(val, nbits):
    reg_w(SWD_REG_WDATA, val)
    reg_w(SWD_REG_RAW_LEN, nbits)
    # HW_EN=1 (bit 15), RAW_MODE=1 (bit 14), START=1 (bit 0)
    reg_w(SWD_REG_CTRL, (1 << 15) | (1 << 14) | 1)
    while reg_r(SWD_REG_STATUS) & 1:
        pass

def swd_op(rnw, apndp, addr, wdata=0, idle=8):
    if not rnw:
        reg_w(SWD_REG_WDATA, wdata)
    # HW_EN=1 (bit 15), idle (12:8), addr (4:3), rnw (2), apndp (1), START (0)
    ctrl = (1 << 15) | (idle << 8) | (addr << 3) | (rnw << 2) | (apndp << 1) | 1
    reg_w(SWD_REG_CTRL, ctrl)
    while reg_r(SWD_REG_STATUS) & 1:
        pass
    st = reg_r(SWD_REG_STATUS)
    ack = (st >> 1) & 7
    parity_err = (st >> 4) & 1
    rdata = reg_r(SWD_REG_RDATA)
    return ack, parity_err, rdata

# Test across various clock dividers:
# 100MHz / (2 * (div + 1))
divs = [
    (4999, "10 kHz"),
    (999,  "50 kHz"),
    (499,  "100 kHz"),
    (249,  "200 kHz"),
    (99,   "500 kHz"),
    (49,   "1.0 MHz"),
    (24,   "2.0 MHz"),
    (9,    "5.0 MHz"),
]

for div, label in divs:
    reg_w(SWD_REG_CLK_DIV, div)

    # 1. Line Reset: >50 ones
    swd_raw_write(0xFFFFFFFF, 32)
    swd_raw_write(0xFFFFFFFF, 32)
    # 2. Switch sequence: 0xE79E (16 bits)
    swd_raw_write(0xE79E, 16)
    # 3. Line Reset: >50 ones
    swd_raw_write(0xFFFFFFFF, 32)
    swd_raw_write(0xFFFFFFFF, 32)
    # 4. Idle: >=8 zeros
    swd_raw_write(0x00000000, 8)

    # 5. Read DPIDR (RnW=1, APnDP=0, Addr=0)
    ack, par_err, dpidr = swd_op(rnw=1, apndp=0, addr=0)
    print(f"HW SWD @ {label:8s} (div={div:4d}) -> ACK={ack}, DPIDR=0x{dpidr:08X}, ParityErr={par_err}")

# Release hardware mux back to EMIO at the end
reg_w(SWD_REG_CTRL, 0)
