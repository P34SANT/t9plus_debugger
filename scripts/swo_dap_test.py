#!/usr/bin/env python3
"""
swo_dap_test.py - Verify the T9+ SWO engine through the CMSIS-DAP v2 command channel.

Sequence:
  1. DAP_INFO(CAPABILITIES)  -> expect SWO UART bit (1<<2)
  2. DAP_INFO(SWO_BUF_SIZE)  -> expect 4096
  3. DAP_SWO_MODE(UART)      -> DAP_OK
  4. DAP_SWO_BAUDRATE(2e6)   -> actual baud ≈ 2_000_000
  5. DAP_SWO_CONTROL(START)  -> DAP_OK
  6. Poll DAP_SWO_STATUS / DAP_SWO_DATA and decode live ITM trace from 407_Blink
"""
import usb.core, usb.util, struct, sys, time
import usb.backend.libusb1
import libusb_package
import argparse
import re
from pathlib import Path

DAP_VID, DAP_PID = 0x0D28, 0x0204

# CMSIS-DAP command IDs (per ARM CMSIS-DAP spec)
ID_DAP_INFO        = 0x00
ID_DAP_SWO_TRANSPORT = 0x17
ID_DAP_SWO_MODE    = 0x18
ID_DAP_SWO_BAUD    = 0x19
ID_DAP_SWO_CONTROL = 0x1A
ID_DAP_SWO_STATUS  = 0x1B
ID_DAP_SWO_DATA    = 0x1C
DAP_INFO_CAPABILITIES = 0xF0
DAP_INFO_SWO_BUF_SIZE = 0xFD

def dap_transfer(dev, cmd: bytes, timeout=2000) -> bytes:
    # Match OpenOCD's full-sized command packets on the USB/IP transport.
    if not cmd or len(cmd) > 512:
        raise ValueError("DAP command must contain 1..512 bytes")
    dev.write(0x01, cmd.ljust(512, b"\0"), timeout)  # EP1 OUT
    reply = bytes(dev.read(0x81, 512, timeout))
    minimum = {0x00: 2, 0x17: 2, 0x18: 2, 0x19: 5,
               0x1A: 2, 0x1B: 6, 0x1C: 4}.get(cmd[0], 1)
    if len(reply) < minimum or reply[0] != cmd[0]:
        raise RuntimeError(f"Invalid DAP 0x{cmd[0]:02X} response: {reply.hex(' ')}")
    if cmd[0] in (0x17, 0x18, 0x1A) and reply[1] != 0:
        raise RuntimeError(f"DAP 0x{cmd[0]:02X} rejected: {reply.hex(' ')}")
    return reply


def decode_itm_port0(data):
    """Extract port-0 software source payloads (not raw UART ASCII)."""
    text = bytearray()
    i = 0
    while i < len(data):
        header = data[i]
        i += 1
        size_code = header & 3
        if size_code:
            size = (1, 2, 4)[size_code - 1]
            if i + size > len(data):
                break  # Capture can end mid-packet.
            if not (header & 4) and header >> 3 == 0:
                text.extend(data[i:i + size])
            i += size
        elif header in (0, 0x80, 0x70):
            continue  # Synchronization padding/terminator or overflow packet.
        elif header & 0x80:
            # Timestamp / extension continuation bytes.
            while i < len(data):
                more = data[i] & 0x80
                i += 1
                if not more:
                    break
    return text.decode("utf-8", errors="replace")


def prepare_swd_target(dev):
    """Wake the ADIv5 debug domain without changing firmware ITM/TPIU setup."""
    dap_transfer(dev, bytes([2, 1]))
    dap_transfer(dev, bytes([0x11]) + struct.pack("<I", 1000000))
    dap_transfer(dev, bytes([0x13, 0]))
    dap_transfer(dev, bytes([4, 0]) + struct.pack("<HH", 1000, 0))
    for bits in (b"\xff" * 7, b"\x9e\xe7", b"\xff" * 7, b"\x00"):
        dap_transfer(dev, bytes([0x12, len(bits) * 8]) + bits)
    reply = dap_transfer(dev, bytes([5, 0, 1, 2]))
    if len(reply) != 7 or reply[1:3] != bytes([1, 1]):
        raise RuntimeError(f"SWD IDCODE failed: {reply.hex(' ')}")
    dap_transfer(dev, bytes([5, 0, 1, 0]) + struct.pack("<I", 0x1E))
    reply = dap_transfer(dev, bytes([5, 0, 1, 4]) + struct.pack("<I", 0x50000000))
    if reply[1:3] != bytes([1, 1]):
        raise RuntimeError(f"Debug power request failed: {reply.hex(' ')}")
    for _ in range(100):
        reply = dap_transfer(dev, bytes([5, 0, 1, 6]))
        if (len(reply) == 7 and reply[1:3] == bytes([1, 1])
                and struct.unpack_from("<I", reply, 3)[0] & 0xA0000000 == 0xA0000000):
            return
        time.sleep(0.01)
    raise RuntimeError("Target debug power-up acknowledgement timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=4)
    parser.add_argument("--output", type=Path, help="Save raw ITM packets")
    parser.add_argument("--allow-live-hardware", action="store_true",
                        help="Explicitly allow USB access and target debug writes")
    args = parser.parse_args()
    if not args.allow_live_hardware:
        parser.error("Live USB/IP testing is paused after a Windows BSOD. "
                     "Explicit approval and --allow-live-hardware are required.")
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    dev = usb.core.find(idVendor=DAP_VID, idProduct=DAP_PID,
                        backend=libusb_package.get_libusb1_backend())
    if dev is None:
        print("[-] Plink CMSIS-DAP device (0x0D28:0x0204) not found. Attach with usbip first.")
        sys.exit(1)

    # Never detach kernel drivers or reset the composite device in this test.
    usb.util.claim_interface(dev, 0)

    print(f"[+] Found {dev.manufacturer!r} {dev.product!r} on bus {dev.bus} dev {dev.address}")
    prepare_swd_target(dev)
    print("[+] Target debug domain awake (firmware trace configuration unchanged)")

    # 1. Capabilities
    r = dap_transfer(dev, bytes([ID_DAP_INFO, DAP_INFO_CAPABILITIES]))
    print(f"    INFO raw = {[hex(x) for x in r]}")
    if len(r) < 3:
        print(f"[-] Short INFO response ({len(r)} bytes): {list(r)}")
        sys.exit(1)
    cap = r[2]
    print(f"[1] CAPABILITIES  = 0x{cap:02X} "
          f"(SWD={'Y' if cap & 1 else 'N'}, SWO_UART={'Y' if cap & 4 else 'N'}, "
          f"SWO_STREAM={'Y' if cap & 64 else 'N'})")
    if not (cap & 4):
        print("[-] SWO UART capability NOT reported — hardware engine missing")
        sys.exit(1)

    # 2. SWO buffer size
    r = dap_transfer(dev, bytes([ID_DAP_INFO, DAP_INFO_SWO_BUF_SIZE]))
    if r[1] != 4 or len(r) != 6:
        raise RuntimeError(f"SWO buffer size must be a 32-bit word: {r.hex(' ')}")
    buf_size = struct.unpack_from("<I", r, 2)[0]
    print(f"[2] SWO_BUF_SIZE  = {buf_size} (expect 4096)")

    dap_transfer(dev, bytes([ID_DAP_SWO_CONTROL, 0]))
    dap_transfer(dev, bytes([ID_DAP_SWO_TRANSPORT, 1]))
    # 3. SWO mode = UART
    r = dap_transfer(dev, bytes([ID_DAP_SWO_MODE, 0x01]))
    print(f"[3] SWO_MODE      = 0x{r[1]:02X} (expect 0x00 = DAP_OK)")

    # 4. Baudrate
    r = dap_transfer(dev, bytes([ID_DAP_SWO_BAUD]) + struct.pack('<I', 2_000_000))
    actual = struct.unpack('<I', bytes(r[1:5]))[0]
    print(f"[4] SWO_BAUDRATE  = {actual} bps (requested 2000000)")

    # 5. Start capture
    r = dap_transfer(dev, bytes([ID_DAP_SWO_CONTROL, 0x01]))
    print(f"[5] SWO_CONTROL(START) = 0x{r[1]:02X} (expect 0x00)")

    # 6. Live capture: collect bytes, verify readable ITM ASCII from 407_Blink
    print(f"[6] Capturing live SWO trace for {args.seconds:g} seconds ...")
    collected = bytearray()
    deadline = time.monotonic() + args.seconds
    last_status = None
    try:
        while time.monotonic() < deadline:
            r = dap_transfer(dev, bytes([ID_DAP_SWO_STATUS]))
            status = r[1]
            count = struct.unpack_from("<I", r, 2)[0]
            if status != last_status:
                print(f"    status=0x{status:02X} (capture={status&1}, overrun={(status>>7)&1}) avail={count}")
                last_status = status
            if count == 0:
                time.sleep(0.01)
                continue
            req = min(count, 400)
            r = dap_transfer(dev, bytes([ID_DAP_SWO_DATA]) + struct.pack('<H', req))
            n = struct.unpack_from("<H", r, 2)[0]
            if n > req or len(r) != 4 + n:
                raise RuntimeError(f"Invalid SWO data count: {n}, response size {len(r)}")
            collected.extend(r[4:])
    finally:
        try:
            dap_transfer(dev, bytes([ID_DAP_SWO_CONTROL, 0]))
        finally:
            usb.util.dispose_resources(dev)

    if args.output:
        args.output.write_bytes(collected)
    print(f"\n[+] Captured {len(collected)} SWO bytes total")
    if collected:
        text = decode_itm_port0(collected)
        print("-" * 60)
        print(text[:2000])
        print("-" * 60)
        hits = text.count("[SWO]")
        pulses = [int(x) for x in re.findall(r"Pulse #(\d+)", text)]
        if hits > 0:
            print(f"[+] VERIFIED: {hits} '[SWO] ITM Trace Pulse' packets decoded from hardware trace!")
            print(f"[+] Pulse numbers observed: {pulses[:10]}{'...' if len(pulses)>10 else ''}")
        else:
            print("[!] Captured data but no [SWO] markers found - check baud/wiring")
            return 1
    else:
        print("[-] No SWO bytes captured - check wiring (PB3 -> J3 Pin 6) and firmware")
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
