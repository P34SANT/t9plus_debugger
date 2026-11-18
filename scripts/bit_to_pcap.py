#!/usr/bin/env python3
"""Validated Vivado bitstream to Zynq PCAP conversion (safe to import)."""
import argparse
from pathlib import Path
import struct


def bit_to_pcap(data):
    pos = 0

    def take(size):
        nonlocal pos
        result = data[pos:pos + size]
        if len(result) != size:
            raise ValueError("Truncated bitstream")
        pos += size
        return result

    def short():
        return struct.unpack(">H", take(2))[0]

    if short() != 9:
        raise ValueError("Invalid Vivado bitstream header")
    take(9)
    if short() != 1 or take(1) != b"a":
        raise ValueError("Invalid bitstream design tag")
    take(short())
    for expected in (b"b", b"c", b"d"):
        if take(1) != expected:
            raise ValueError("Invalid bitstream metadata order")
        take(short())
    if take(1) != b"e":
        raise ValueError("Missing bitstream payload")
    size = struct.unpack(">I", take(4))[0]
    if not size or size % 4:
        raise ValueError("Bitstream payload must contain whole words")
    payload = take(size)
    if pos != len(data):
        raise ValueError("Unexpected trailing bitstream bytes")
    return b"".join(payload[i:i + 4][::-1] for i in range(0, size, 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    converted = bit_to_pcap(args.source.read_bytes())
    args.destination.write_bytes(converted)
    print(f"Converted {len(converted)} payload bytes")


if __name__ == "__main__":
    main()
