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

def swd_raw_write(val, nbits):
    reg_w(SWD_REG_WDATA, val)
    reg_w(SWD_REG_RAW_LEN, nbits)
    reg_w(SWD_REG_CTRL, (1 << 15) | (1 << 14) | 1)
    while reg_r(SWD_REG_STATUS) & 1:
        pass

def swd_op(rnw, apndp, addr, wdata=0, idle=8):
    if not rnw:
        reg_w(SWD_REG_WDATA, wdata)
    ctrl = (1 << 15) | (idle << 8) | (addr << 3) | (rnw << 2) | (apndp << 1) | 1
    reg_w(SWD_REG_CTRL, ctrl)
    while reg_r(SWD_REG_STATUS) & 1:
        pass
    st = reg_r(SWD_REG_STATUS)
    ack = (st >> 1) & 7
    par_err = (st >> 4) & 1
    rdata = reg_r(SWD_REG_RDATA)
    return ack, par_err, rdata

divs = [1, 2, 4, 7, 9, 14, 19, 24, 29, 39, 49, 74, 99, 199, 499]

for div in divs:
    reg_w(SWD_REG_CLK_DIV, div)
    freq_mhz = 100.0 / (2 * (div + 1))
    success = 0
    total = 20
    for _ in range(total):
        swd_raw_write(0xFFFFFFFF, 32)
        swd_raw_write(0xFFFFFFFF, 32)
        swd_raw_write(0xE79E, 16)
        swd_raw_write(0xFFFFFFFF, 32)
        swd_raw_write(0xFFFFFFFF, 32)
        swd_raw_write(0x00000000, 8)
        ack, par, dpidr = swd_op(rnw=1, apndp=0, addr=0)
        if ack == 1 and dpidr == 0x2BA01477 and par == 0:
            success += 1
    print(f"div={div:3d} ({freq_mhz:6.2f} MHz): {success:2d}/{total} OK | last: ACK={ack}, par={par}, DPIDR=0x{dpidr:08X}")

reg_w(SWD_REG_CTRL, 0)
