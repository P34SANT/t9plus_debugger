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
        shutil.copyfile(filepath, f"/lib/firmware/{target_fw}")
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
    else:
        print(f"[-] WARNING: FPGA state is '{state}'. Check dmesg for details.")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: sudo ./t9_load_bit.py <design.bit or design_pcap.bin>")
        sys.exit(1)
    load_bit(sys.argv[1])
