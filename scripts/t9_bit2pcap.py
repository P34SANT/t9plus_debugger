#!/usr/bin/env python3
"""
t9_bit2pcap.py - Convert Xilinx Vivado .bit files to Zynq PCAP/fpga_manager compatible .bin
Performs header stripping and 32-bit word byte-swapping required by Linux zynq-fpga driver.
"""
import sys
import os
import struct

def bit_to_pcap(bitfile_path, binfile_path=None):
    if not os.path.isfile(bitfile_path):
        raise FileNotFoundError(f"Bitfile not found: {bitfile_path}")

    if binfile_path is None:
        binfile_path = os.path.splitext(bitfile_path)[0] + "_pcap.bin"

    short = struct.Struct('>H')
    ulong = struct.Struct('>I')
    sl = struct.Struct('<I')
    sb = struct.Struct('>I')

    with open(bitfile_path, 'rb') as f:
        # Check standard Xilinx bit header length (0x0009)
        l = short.unpack(f.read(2))[0]
        if l != 9:
            raise ValueError(f"Invalid bit header length: {l}")
        f.read(l) # dummy sync
        short.unpack(f.read(2))[0] # section 'a' length
        if f.read(1) != b'a':
            raise ValueError("Missing 'a' header")
        l = short.unpack(f.read(2))[0]
        design_name = f.read(l)
        print(f"[*] Design Name: {design_name.decode('latin1', errors='ignore').strip()}")

        while True:
            k = f.read(1)
            if not k:
                raise ValueError("Unexpected EOF before data section 'e'")
            if k == b'e':
                data_len = ulong.unpack(f.read(4))[0]
                print(f"[*] Bitstream Payload Length: {data_len} bytes")
                raw_data = f.read(data_len)
                break
            else:
                l = short.unpack(f.read(2))[0]
                metadata = f.read(l)
                key_name = {b'b': 'Part', b'c': 'Date', b'd': 'Time'}.get(k, k.decode('latin1'))
                print(f"[*] {key_name}: {metadata.decode('latin1', errors='ignore').strip()}")

    # Flip 32-bit endianness for Zynq PCAP DMA
    print(f"[*] Swapping 32-bit word endianness for Linux zynq-fpga manager...")
    flipped = bytearray(len(raw_data))
    for i in range(0, len(raw_data), 4):
        sb.pack_into(flipped, i, sl.unpack_from(raw_data, i)[0])

    # Verify sync word (0x66 0x55 0x99 0xAA)
    sync_idx = flipped.find(b'\x66\x55\x99\xaa')
    if sync_idx == -1:
        raise ValueError("CRITICAL: Zynq FPGA sync word (66 55 99 aa) not found in converted binary!")
    print(f"[+] Verified Zynq sync word at byte offset {sync_idx}")

    with open(binfile_path, 'wb') as fout:
        fout.write(flipped)
    print(f"[+] Successfully generated PCAP binary: {binfile_path} ({len(flipped)} bytes)")
    return binfile_path

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python t9_bit2pcap.py <input.bit> [output.bin]")
        sys.exit(1)
    out_bin = sys.argv[2] if len(sys.argv) > 2 else None
    bit_to_pcap(sys.argv[1], out_bin)
