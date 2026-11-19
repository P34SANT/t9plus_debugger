#!/usr/bin/env python3
"""Checked native UARTLite module/overlay lifecycle; plan never touches hardware."""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import time

PACKAGE = Path(__file__).resolve().parent
DT = Path("/sys/firmware/devicetree/base")
OVERLAY = Path("/sys/kernel/config/device-tree/overlays/plink-uartlite")
MARKER = Path("/run/plink/uartlite-native.json")
EXPECTED_RELEASE = "6.6.0-xilinx-ge475cc1c9432"
KERNEL_COMMIT = "e475cc1c9432587298212570c2af2be66b820c41"


def run(*args):
    result = subprocess.run(args, check=False, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(" ".join(args) + ": " + result.stderr.strip())
    return result.stdout.strip()


def verify_package(package=PACKAGE):
    manifest = json.loads((package / "manifest.json").read_text())
    if (manifest.get("kernel_release") != EXPECTED_RELEASE or
            manifest.get("kernel_commit") != KERNEL_COMMIT):
        raise ValueError("Wrong driver kernel identity")
    required = {"uartlite.ko", "activate.py", "native_bridge.py",
                "plink-uartlite.service", "native-bridge.conf"}
    if not required.issubset(manifest.get("files", {})):
        raise ValueError("Incomplete native driver package")
    for name, expected in manifest["files"].items():
        if not name or Path(name).name != name or name in (".", ".."):
            raise ValueError("Unsafe manifest filename")
        if hashlib.sha256((package / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Driver package mismatch: " + name)
    return manifest


def cell(path):
    data = path.read_bytes()
    if len(data) != 4:
        raise ValueError("Expected one device-tree cell: " + str(path))
    return struct.unpack(">I", data)[0]


def overlay_source(tree=DT):
    bus = tree / "axi"
    clock = bus / "slcr@f8000000/clkc@100"
    controller = bus / "interrupt-controller@f8f01000"
    if cell(bus / "#address-cells") != 1 or cell(bus / "#size-cells") != 1:
        raise ValueError("Unsupported AXI bus cell sizes")
    if b"xlnx,ps7-clkc" not in (clock / "compatible").read_bytes():
        raise ValueError("Unexpected Zynq clock provider")
    if b"arm,cortex-a9-gic" not in (controller / "compatible").read_bytes():
        raise ValueError("Unexpected interrupt controller")
    if cell(clock / "#clock-cells") != 1 or cell(controller / "#interrupt-cells") != 3:
        raise ValueError("Unsupported clock/interrupt cells")
    clock_handle = cell(clock / "phandle")
    irq_handle = cell(controller / "phandle")
    if not clock_handle or not irq_handle:
        raise ValueError("Missing live device-tree handles")
    return f"""/dts-v1/;
/plugin/;
/ {{
    fragment@0 {{
        target-path = "/";
        __overlay__ {{
            plink_uartlite_clock: plink-uartlite-clock {{
                compatible = "fixed-clock";
                #clock-cells = <0>;
                clock-frequency = <100000000>;
                clock-output-names = "plink-uartlite-100m";
            }};
        }};
    }};
    fragment@1 {{
        target-path = "/axi";
        __overlay__ {{
            #address-cells = <1>;
            #size-cells = <1>;
            serial@42c00000 {{
                compatible = "xlnx,xps-uartlite-1.00.a";
                reg = <0x42c00000 0x10000>;
                clocks = <&plink_uartlite_clock>;
                clock-names = "s_axi_aclk";
                interrupt-parent = <0x{irq_handle:x}>;
                interrupts = <0 29 1>;
                current-speed = <115200>;
                xlnx,use-parity = <0>;
                xlnx,data-bits = <8>;
                port-number = <0>;
                status = "okay";
            }};
        }};
    }};
}};
"""


def ensure_bridge_stopped():
    result = subprocess.run(["systemctl", "is-active", "--quiet", "t9-uart-bridge"],
                            check=False, timeout=10)
    if result.returncode == 0:
        raise RuntimeError("Stop the UART bridge before changing its kernel driver")


def identity():
    manifest = verify_package()
    if os.uname().release != manifest["kernel_release"]:
        raise RuntimeError("Refusing a module built for another running kernel")
    return manifest["files"]["uartlite.ko"]


def verify_native_tty():
    device = Path("/sys/class/tty/ttyUL0/device")
    if not Path("/dev/ttyUL0").exists():
        raise RuntimeError("Native UARTLite TTY is missing")
    if (device / "driver").resolve().name != "uartlite":
        raise RuntimeError("ttyUL0 is not bound to UARTLite")
    node = (device / "of_node").resolve()
    if node.name != "serial@42c00000":
        raise RuntimeError("ttyUL0 is not the intended FPGA UART")


def start():
    module_hash = identity()
    if MARKER.exists():
        marker = json.loads(MARKER.read_text())
        if marker.get("module_sha256") != module_hash:
            raise RuntimeError("A different native driver owns this session")
        verify_native_tty()
        return
    ensure_bridge_stopped()
    if Path("/sys/module/uartlite").exists() or OVERLAY.exists():
        raise RuntimeError("Unowned UARTLite module/overlay already exists")
    if (Path("/run/plink/fpga-ready").read_text() != "plink-abi-2\n" or
            Path("/sys/class/fpga_manager/fpga0/state").read_text().strip() != "operating"):
        raise RuntimeError("A verified operating ABI 2 FPGA is required")
    if (DT / "axi/serial@42c00000").exists():
        raise RuntimeError("An existing device-tree node already owns UART1")
    if (DT / "plink-uartlite-clock").exists():
        raise RuntimeError("An existing node already owns the native clock description")
    # Check the existing release's core IDs, not arbitrary MMIO ranges.
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        with mmap.mmap(fd, 4096, access=mmap.ACCESS_READ, offset=0xF8000000) as registers:
            if struct.unpack_from("<I", registers, 0x170)[0] != 0x00100A00:
                raise RuntimeError("FCLK0 no longer matches the verified 100 MHz configuration")
        for address, expected in ((0x43C00000, 0x53574402), (0x43C20000, 0x53574F02)):
            with mmap.mmap(fd, 4096, access=mmap.ACCESS_READ, offset=address) as registers:
                if struct.unpack_from("<I", registers, 0x18)[0] != expected:
                    raise RuntimeError("FPGA identity mismatch")
    finally:
        os.close(fd)
    work = Path("/run/plink")
    source = work / "uartlite-native.dts"
    binary = work / "uartlite-native.dtbo"
    source.write_text(overlay_source())
    # The factory base DT has no __symbols__. We use target paths and local
    # references, so do not export overlay symbols with dtc -@.
    run("dtc", "-I", "dts", "-O", "dtb", "-o", str(binary), str(source))
    loaded = False
    created = False
    try:
        run("insmod", str(PACKAGE / "uartlite.ko"))  # Never force version checks.
        loaded = True
        OVERLAY.mkdir()
        created = True
        (OVERLAY / "dtbo").write_bytes(binary.read_bytes())
        if (OVERLAY / "status").read_text().strip() != "applied":
            raise RuntimeError("The live kernel rejected the UART device-tree overlay")
        deadline = time.monotonic() + 5
        while not Path("/dev/ttyUL0").exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        verify_native_tty()
        temporary = MARKER.with_suffix(".new")
        temporary.write_text(json.dumps({"schema": 1, "module_sha256": module_hash,
                                        "kernel_release": os.uname().release}) + "\n")
        temporary.replace(MARKER)
    except Exception:
        if created:
            OVERLAY.rmdir()
        if loaded:
            run("rmmod", "uartlite")
        raise


def stop():
    ensure_bridge_stopped()
    if not MARKER.exists():
        if OVERLAY.exists() or Path("/sys/module/uartlite").exists():
            raise RuntimeError("Refusing to remove an unowned module/overlay")
        return
    if json.loads(MARKER.read_text()).get("module_sha256") != identity():
        raise RuntimeError("Driver ownership mismatch")
    if OVERLAY.exists():
        OVERLAY.rmdir()
    if Path("/sys/module/uartlite").exists():
        run("rmmod", "uartlite")
    MARKER.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "verify", "start", "stop"))
    args = parser.parse_args()
    if args.action == "plan":
        print(overlay_source())
        return
    verify_package()
    if args.action == "verify":
        print("Native driver package: verified")
        return
    if os.geteuid() != 0:
        parser.error("Driver changes require root")
    {"start": start, "stop": stop}[args.action]()
    print("Native UARTLite: " + args.action + " complete")


if __name__ == "__main__":
    main()
