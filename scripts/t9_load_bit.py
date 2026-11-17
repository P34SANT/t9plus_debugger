#!/usr/bin/env python3
"""
t9_load_bit.py - Load bitstream into Antminer T9+ Zynq PL via Linux fpga_manager
Usage: sudo ./t9_load_bit.py <bitstream.bit or bitstream_pcap.bin>
"""
import sys
import os
import shutil
import subprocess

def load_bit(filepath):
    if not os.path.isfile(filepath):
        print(f"ERROR: File not found: {filepath}", file=sys.stderr)
        sys.exit(1)

    base = os.path.basename(filepath)
    if filepath.endswith('.bit'):
        print(f"[*] Input is Vivado .bit file. Converting to PCAP format...")
        # Import conversion function
        import struct
        short = struct.Struct('>H')
        ulong = struct.Struct('>I')
        sl = struct.Struct('<I')
        sb = struct.Struct('>I')

        with open(filepath, 'rb') as f:
            l = short.unpack(f.read(2))[0]
            if l != 9:
                raise ValueError("Invalid .bit header")
            f.read(l)
            short.unpack(f.read(2))[0]
            f.read(1)
            l = short.unpack(f.read(2))[0]
            f.read(l)
            while True:
                k = f.read(1)
                if not k:
                    raise ValueError("Unexpected EOF in .bit file")
                if k == b'e':
                    data_len = ulong.unpack(f.read(4))[0]
                    raw_data = f.read(data_len)
                    break
                else:
                    l = short.unpack(f.read(2))[0]
                    f.read(l)

        flipped = bytearray(len(raw_data))
        for i in range(0, len(raw_data), 4):
            sb.pack_into(flipped, i, sl.unpack_from(raw_data, i)[0])

        pcap_name = f"{os.path.splitext(base)[0]}_pcap.bin"
        fw_path = f"/lib/firmware/{pcap_name}"
        os.makedirs("/lib/firmware", exist_ok=True)
        with open(fw_path, 'wb') as fout:
            fout.write(flipped)
        target_fw = pcap_name
    elif filepath.endswith('.bin'):
        target_fw = base
        os.makedirs("/lib/firmware", exist_ok=True)
        dst_fw = f"/lib/firmware/{target_fw}"
        if os.path.abspath(filepath) != os.path.abspath(dst_fw):
            shutil.copyfile(filepath, dst_fw)
    else:
        print(f"ERROR: Unrecognized file extension: {filepath}. Expected .bit or .bin", file=sys.stderr)
        sys.exit(1)

    print(f"[*] Installed firmware to /lib/firmware/{target_fw}")
    print(f"[*] Programming Zynq PL via /sys/class/fpga_manager/fpga0/firmware...")

    try:
        with open("/sys/class/fpga_manager/fpga0/firmware", "w") as f:
            f.write(target_fw)
    except Exception as e:
        print(f"ERROR: Failed writing to fpga_manager firmware node: {e}", file=sys.stderr)
        sys.exit(1)

    # Check state
    state = "unknown"
    if os.path.exists("/sys/class/fpga_manager/fpga0/state"):
        with open("/sys/class/fpga_manager/fpga0/state", "r") as f:
            state = f.read().strip()

    if state == "operating":
        print(f"[+] SUCCESS: FPGA PL configured and state is 'operating'!")
        print(f"[+] D1 (FPGA_DONE) LED should now be GREEN.")
        configure_fclk0_100mhz()
    else:
        print(f"[-] WARNING: FPGA state is '{state}'. Check dmesg for details.")

def configure_fclk0_100mhz():
    """
    Ensure Zynq SLCR FPGA0_CLK_CTRL is set to 100.000 MHz.
    The factory Antminer FSBL initializes FCLK0 with Divisor=20 (50 MHz).
    Our Vivado PL design (AXI UARTLite, AXI SWD Engine, AXI Interconnect)
    is synthesized for 100 MHz. Setting Divisor0=10 (0x0A) provides exactly
    1000 MHz / 10 = 100.000 MHz, bringing AXI UART1 to accurate 115,200 baud
    and SWD clock dividers to their true intended frequencies.
    """
    try:
        import mmap, struct
        SLCR_BASE = 0xF8000000
        SLCR_LOCK = 0x004
        SLCR_UNLOCK = 0x008
        FPGA0_CLK_CTRL = 0x170

        with open('/dev/mem', 'r+b') as f:
            mm = mmap.mmap(f.fileno(), 4096, offset=SLCR_BASE)
            # Unlock SLCR (0xDF0D)
            mm[SLCR_UNLOCK:SLCR_UNLOCK+4] = struct.pack('<I', 0xDF0D)
            # Divisor0=10 (0x0A), Divisor1=1 (0x01), SRC=IO_PLL (0x00) -> 0x00100A00 (100.0 MHz)
            mm[FPGA0_CLK_CTRL:FPGA0_CLK_CTRL+4] = struct.pack('<I', 0x00100A00)
            # Lock SLCR (0x767B)
            mm[SLCR_LOCK:SLCR_LOCK+4] = struct.pack('<I', 0x767B)
            val = struct.unpack('<I', mm[FPGA0_CLK_CTRL:FPGA0_CLK_CTRL+4])[0]
            mm.close()
            print(f"[+] Configured Zynq SLCR FCLK0 to 100.0 MHz (reg=0x{val:08X})")
    except Exception as e:
        print(f"[-] Warning: Unable to adjust SLCR FCLK0 to 100 MHz: {e}", file=sys.stderr)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: sudo ./t9_load_bit.py <design.bit or design_pcap.bin>")
        sys.exit(1)
    load_bit(sys.argv[1])
