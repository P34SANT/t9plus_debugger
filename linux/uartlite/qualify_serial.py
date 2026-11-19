#!/usr/bin/env python3
"""Bounded pulse comparison after discarding transport backlog; no firmware flash."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import urllib.request

import serial

LINE = re.compile(rb"\[USART1\] STM32 Pulse #(\d+) \| Tick: (\d+) ms")
MAX_BYTES = 8 * 1024 * 1024


def metrics():
    with urllib.request.urlopen("http://192.168.2.99:8080/api/metrics", timeout=5) as response:
        return json.load(response)


def capture(port, warmup, seconds, ready):
    discarded = 0
    data = bytearray()
    with serial.Serial(port, 115200, timeout=0.05) as device:
        deadline = time.monotonic() + warmup
        while time.monotonic() < deadline:
            discarded += len(device.read(min(device.in_waiting or 1, 4096)))
        ready.wait(timeout=warmup + 15)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            data.extend(device.read(min(device.in_waiting or 1, 4096)))
            if len(data) > MAX_BYTES:
                raise RuntimeError("Capture exceeded bounded byte capacity")
    return bytes(data), discarded


def analyze(data):
    lines = data.split(b"\r\n")
    ids = []
    malformed = 0
    for index, line in enumerate(lines):
        if not line:
            continue
        match = LINE.fullmatch(line)
        if match:
            ids.append(int(match[1]))
        elif index == 0 and not data.startswith(b"[USART1] "):
            continue  # Capture began in the middle of a line.
        elif index == len(lines) - 1 and not data.endswith(b"\r\n"):
            continue  # Capture ended in the middle of a line.
        else:
            malformed += 1
    return {"bytes": len(data), "complete_pulses": len(ids),
            "malformed_lines": malformed,
            "consecutive": all(b == a + 1 for a, b in zip(ids, ids[1:])),
            "ids": ids,
            "ticks": [int(match[2]) for line in lines
                      if (match := LINE.fullmatch(line))]}


def error_deltas(baseline, after, include_swo=False):
    """Unknown/stale metrics cannot qualify traffic as loss-free."""
    before = baseline.get("uart", {})
    current = after.get("uart", {})
    uart_before = before.get("metrics", {}).get("uart1", {})
    uart_after = current.get("metrics", {}).get("uart1", {})
    errors = []
    deltas = {}
    for snapshot, uart in ((before, uart_before), (current, uart_after)):
        age = snapshot.get("age_seconds")
        if (not isinstance(age, (int, float)) or not 0 <= age <= 3 or
                uart.get("backend") != "kernel_tty_irq" or
                uart.get("counter_state") != "measured"):
            errors.append("Fresh native IRQ metrics and measured kernel counters are required")
    for key in ("hardware_overruns", "kernel_buffer_overruns", "framing_errors", "parity_errors"):
        if key not in uart_after or key not in uart_before:
            errors.append("Missing UART counter: " + key)
        else:
            deltas[key] = (uart_after[key] - uart_before[key]) & 0xFFFFFFFF
    if any(deltas.values()):
        errors.append("Kernel UART error counters increased")
    if include_swo:
        pod = after.get("pod1", {})
        age = pod.get("age_seconds")
        swo = pod.get("metrics", {}).get("swo", {})
        if not isinstance(age, (int, float)) or not 0 <= age <= 3:
            errors.append("Fresh SWO metrics are required")
        # SWO start resets capture counters; require measured zero, not absent.
        for key in ("software_dropped", "hardware_dropped", "framing_errors"):
            if swo.get(key) != 0:
                errors.append("Missing or nonzero SWO counter: " + key)
    return deltas, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-live-hardware", action="store_true")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--warmup", type=float, default=5)
    parser.add_argument("--ports", nargs="+", default=["COM6", "COM10"])
    parser.add_argument("--swo", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.allow_live_hardware:
        parser.error("Explicit live-hardware approval is required")
    if not 1 <= args.seconds <= 300 or not 1 <= args.warmup <= 30:
        parser.error("Use 1..300 seconds of capture and 1..30 seconds of warmup")
    if len(set(args.ports)) != len(args.ports) or not 1 <= len(args.ports) <= 3:
        parser.error("Use one to three distinct serial ports")
    args.output.mkdir(parents=True, exist_ok=False)
    started = datetime.now(timezone.utc).isoformat()
    monotonic_start = time.monotonic()
    baseline = metrics()
    ready = threading.Barrier(len(args.ports) + 1)
    duration = args.seconds + (2 if args.swo else 0)
    with ThreadPoolExecutor(max_workers=len(args.ports)) as pool:
        futures = {port: pool.submit(capture, port, args.warmup, duration, ready)
                   for port in args.ports}
        ready.wait(timeout=args.warmup + 20)
        if args.swo:
            root = Path(__file__).resolve().parents[2]
            command = [sys.executable, str(root / "scripts/swo_dap_test.py"),
                       "--allow-live-hardware", "--seconds", str(args.seconds),
                       "--output", str(args.output / "swo.bin")]
            result = subprocess.run(command, check=False, timeout=args.seconds + 20)
            if result.returncode:
                raise RuntimeError("Concurrent SWO capture failed")
        serial_results = {}
        for port, future in futures.items():
            data, discarded = future.result(timeout=duration + 10)
            (args.output / f"{port}.bin").write_bytes(data)
            serial_results[port] = dict(analyze(data), warmup_discarded=discarded)
    after = metrics()
    failed = any(item["complete_pulses"] < 2 or item["malformed_lines"] or not item["consecutive"]
                 for item in serial_results.values())
    common = set.intersection(*(set(item["ids"]) for item in serial_results.values()))
    failed |= len(common) < 2
    # All captures must contain every pulse in their shared time interval,
    # with matching payload ticks, not merely any two overlapping IDs.
    payloads = [dict(zip(item["ids"], item["ticks"])) for item in serial_results.values()]
    if all(payloads):
        lower = max(min(item) for item in payloads)
        upper = min(max(item) for item in payloads)
        failed |= lower > upper
        failed |= any(len({item.get(pulse) for item in payloads}) != 1
                      for pulse in range(lower, upper + 1))
    report = {"schema": 1, "host_started_utc": started,
              "seconds": duration, "warmup_seconds": args.warmup,
              "serial": serial_results, "common_pulses": len(common),
              "metrics_before": baseline, "metrics_after": after}
    if args.swo:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
        from swo_dap_test import decode_itm_port0
        text = decode_itm_port0((args.output / "swo.bin").read_bytes())
        ids = [int(value) for value in re.findall(r"\[SWO\] ITM Trace Pulse #(\d+)", text)]
        report["swo"] = {"pulses": len(ids), "ids": ids,
                         "consecutive": all(b == a + 1 for a, b in zip(ids, ids[1:])),
                         "common_with_serial": len(set(ids) & common)}
        failed |= len(ids) < 2 or not report["swo"]["consecutive"] or not set(ids).issubset(common)
    report["kernel_error_deltas"], report["metric_errors"] = error_deltas(
        baseline, after, args.swo)
    failed |= bool(report["metric_errors"])
    report["host_elapsed_seconds"] = time.monotonic() - monotonic_start
    report["passed"] = not failed
    (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    summary = {**report, "metrics_before": "see report.json",
               "metrics_after": "see report.json"}
    summary["serial"] = {
        port: {**{key: value for key, value in item.items() if key not in ("ids", "ticks")},
               "id_range": [item["ids"][0], item["ids"][-1]] if item["ids"] else []}
        for port, item in serial_results.items()}
    if args.swo:
        summary["swo"] = {key: value for key, value in report["swo"].items() if key != "ids"}
    print(json.dumps(summary, indent=2))
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
