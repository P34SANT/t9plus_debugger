#!/usr/bin/env python3
"""Load a verified Plink release. All MMIO clients MUST be stopped first."""
import argparse
import ctypes
import mmap
import os
from pathlib import Path
import shutil
import subprocess

from release_manifest import verify

CLIENTS = ("t9-cmsis-dap", "t9-uart-bridge", "t9-io-daemon",
           "t9-openocd", "plink-stlink", "t9-programmer")
READY = Path("/run/plink/fpga-ready")


def configure_fclk0_100mhz():
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        with mmap.mmap(fd, 4096, offset=0xF8000000) as registers:
            def write(offset, value):
                ctypes.c_uint32.from_buffer(registers, offset).value = value
            write(0x008, 0xDF0D)
            try:
                write(0x170, 0x00100A00)
                value = ctypes.c_uint32.from_buffer(registers, 0x170).value
            finally:
                write(0x004, 0x767B)
            if value != 0x00100A00:
                raise RuntimeError(f"FCLK0 readback mismatch: 0x{value:08X}")
    finally:
        os.close(fd)


def load_bit(filepath):
    filepath = Path(filepath).resolve()
    if filepath.name != "t9_programmer_pcap.bin":
        raise ValueError("Load only the PCAP from a verified release directory")
    verify(filepath.parent, filepath.parent.parent)
    for service in CLIENTS:
        if subprocess.run(["systemctl", "is-active", "--quiet", service],
                          check=False).returncode == 0:
            raise RuntimeError(f"Refusing FPGA reload while {service} is active")
    READY.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    READY.unlink(missing_ok=True)
    firmware = Path("/lib/firmware/plink-release.bin")
    firmware.parent.mkdir(parents=True, exist_ok=True)
    temporary = firmware.with_suffix(".bin.new")
    shutil.copyfile(filepath, temporary)
    temporary.replace(firmware)
    Path("/sys/class/fpga_manager/fpga0/firmware").write_text(firmware.name)
    state = Path("/sys/class/fpga_manager/fpga0/state").read_text().strip()
    if state != "operating":
        raise RuntimeError(f"FPGA state is {state!r}")
    configure_fclk0_100mhz()
    temporary = READY.with_suffix(".new")
    temporary.write_text("plink-abi-2\n")
    temporary.replace(READY)
    print("Verified Plink ABI 2 loaded, FCLK0 readback = 100 MHz configuration")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcap", type=Path)
    parser.add_argument("--allow-live-hardware", action="store_true")
    args = parser.parse_args()
    if not args.allow_live_hardware:
        parser.error("FPGA reload requires explicit approval and --allow-live-hardware")
    load_bit(args.pcap)


if __name__ == "__main__":
    main()
