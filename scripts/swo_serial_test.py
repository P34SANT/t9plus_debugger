#!/usr/bin/env python3
"""Capture SWO and native Windows COM ports concurrently; compare pulse IDs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import serial

from swo_dap_test import decode_itm_port0


def capture_serial(port, seconds):
    with serial.Serial(port, 115200, timeout=0.05) as device:
        device.reset_input_buffer()
        deadline = time.monotonic() + seconds
        data = bytearray()
        while time.monotonic() < deadline:
            data.extend(device.read(device.in_waiting or 1))
        return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", nargs="+", default=["COM6", "COM10"])
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(tempfile.gettempdir()) / "opencode")
    parser.add_argument("--allow-live-hardware", action="store_true",
                        help="Explicitly allow native COM and USB/IP traffic")
    args = parser.parse_args()
    if not args.allow_live_hardware:
        parser.error("Live testing is paused after a Windows BSOD. "
                     "Explicit approval and --allow-live-hardware are required.")
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / "plink-swo-concurrent.bin"
    with ThreadPoolExecutor(max_workers=len(args.serial)) as pool:
        reads = {port: pool.submit(capture_serial, port, args.seconds + 2)
                 for port in args.serial}
        result = subprocess.run([
            sys.executable, str(Path(__file__).with_name("swo_dap_test.py")),
            "--allow-live-hardware",
            "--seconds", str(args.seconds), "--output", str(raw_path),
        ], check=False)
        if result.returncode:
            return result.returncode
        text = decode_itm_port0(raw_path.read_bytes())
        swo_ids = set(re.findall(r"\[SWO\] ITM Trace Pulse #(\d+)", text))
        failed = False
        for port, future in reads.items():
            data = future.result()
            (args.output_dir / f"plink-{port}.bin").write_bytes(data)
            lines = data.split(b"\r\n")
            valid = [re.fullmatch(rb"\[USART1\] STM32 Pulse #(\d+) \| Tick: \d+ ms", line)
                     for line in lines[1:-1] if line]
            bad = sum(match is None for match in valid)
            ids = {match[1].decode("ascii") for match in valid if match}
            overlap = len(ids & swo_ids)
            print(f"{port}: {len(data)} bytes, {len(ids)} complete pulses, "
                  f"{bad} malformed lines, {overlap} pulse IDs shared with SWO")
            failed |= bad > 0 or overlap < 2
    print(f"Evidence: {args.output_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
