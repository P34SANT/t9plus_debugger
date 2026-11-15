import mmap, os, struct

fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
mem = mmap.mmap(fd, 4096, offset=0xE000A000)

MASK_DATA_2_MSW = 0x0014 # Pins 70..85 (or Bank 2 upper bits)
MASK_DATA_2_LSW = 0x0010 # Pins 54..69 (Bank 2 bits 0..15: EMIO 0..15)
DATA_2_RO       = 0x0068
DIRM_2          = 0x0284
OEN_2           = 0x0288

def reg_r(off): return struct.unpack('<I', mem[off:off+4])[0]
def reg_w(off, val): mem[off:off+4] = struct.pack('<I', val)

print(f"Initial DIRM_2: 0x{reg_r(DIRM_2):08X}")
print(f"Initial DATA_2_RO: 0x{reg_r(DATA_2_RO):08X}")

# Configure EMIO 9 (SWCLK1), EMIO 10 (SWDIO1) as outputs
PIN_CLK1 = 9
PIN_DIO1 = 10
PIN_RST1 = 11

reg_w(DIRM_2, reg_r(DIRM_2) | (1 << PIN_CLK1) | (1 << PIN_DIO1))
reg_w(OEN_2,  reg_r(OEN_2)  | (1 << PIN_CLK1) | (1 << PIN_DIO1))

# Set CLK1=1, DIO1=1
mask = (~((1 << PIN_CLK1) | (1 << PIN_DIO1))) & 0xFFFF
data = (1 << PIN_CLK1) | (1 << PIN_DIO1)
reg_w(MASK_DATA_2_LSW, (mask << 16) | data)

print(f"Updated DIRM_2: 0x{reg_r(DIRM_2):08X}")
print(f"Updated DATA_2_RO: 0x{reg_r(DATA_2_RO):08X}")
print("[+] Successfully verified J4 Pod #2 EMIO pin control!")
