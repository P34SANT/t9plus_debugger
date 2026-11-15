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
    while reg_r(SWD_REG_STATUS) & 1: pass

def swd_op(rnw, apndp, addr, wdata=0, idle=8):
    if not rnw: reg_w(SWD_REG_WDATA, wdata)
    ctrl = (1 << 15) | (idle << 8) | (addr << 3) | (rnw << 2) | (apndp << 1) | 1
    reg_w(SWD_REG_CTRL, ctrl)
    while reg_r(SWD_REG_STATUS) & 1: pass
    st = reg_r(SWD_REG_STATUS)
    ack = (st >> 1) & 7
    par_err = (st >> 4) & 1
    rdata = reg_r(SWD_REG_RDATA)
    return ack, par_err, rdata

print("[*] FPGA Engine ID:", hex(reg_r(SWD_REG_ID)))

# 25 MHz (div=1)
reg_w(SWD_REG_CLK_DIV, 1)

# Line reset
swd_raw_write(0xFFFFFFFF, 32)
swd_raw_write(0xFFFFFFFF, 32)
swd_raw_write(0xE79E, 16)
swd_raw_write(0xFFFFFFFF, 32)
swd_raw_write(0xFFFFFFFF, 32)
swd_raw_write(0x00000000, 8)

TOTAL = 5000
errors = 0
t0 = time.time()

print(f"[*] Starting stress test: {TOTAL} consecutive SWD operations at 25.0 MHz...")
for i in range(TOTAL):
    ack, par, rdata = swd_op(1, 0, 0) # DPIDR
    if ack != 1 or par != 0 or rdata != 0x2BA01477:
        errors += 1
        if errors <= 5:
            print(f"[-] Iteration {i} FAIL: ack={ack}, par={par}, rdata={hex(rdata)}")

t1 = time.time()
elapsed = t1 - t0
rate = TOTAL / elapsed

print(f"[+] COMPLETED: {TOTAL - errors}/{TOTAL} Passed ({(TOTAL - errors)/TOTAL*100:.2f}%)")
print(f"[+] Total Time: {elapsed:.3f}s | Rate: {rate:.1f} ops/sec | Mean latency: {elapsed/TOTAL*1000000:.1f} µs/op")
if errors == 0:
    print("[+] 100% ROCK SOLID STABILITY CONFIRMED AT 25.0 MHZ!")
else:
    print(f"[-] Failed with {errors} errors.")
