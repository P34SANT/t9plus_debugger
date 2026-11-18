#!/usr/bin/env python3
"""Fail offline if canonical pin mapping, XDC, GPIO configuration or C diverge."""
import json
from pathlib import Path
import re


def check(root):
    mapping = json.loads((root / "hardware/pinmap.json").read_text())
    gpio = json.loads((root / "t9_programmer/io_daemon/pins.json").read_text())
    xdc = (root / "constraints/t9_programmer.xdc").read_text()
    assigned = dict((port, pin) for pin, port in re.findall(
        r"set_property PACKAGE_PIN (\w+) \[get_ports \{(\w+)\}\]", xdc))
    seen = set()
    for pin in mapping["pins"]:
        physical = pin["header"], pin["pin"]
        if physical in seen:
            raise ValueError(f"Duplicate physical pin: {physical}")
        seen.add(physical)
        if assigned.get(pin["port"]) != pin["fpga"]:
            raise ValueError(f"XDC mismatch: {pin['name']}")
        if pin["name"] in gpio:
            entry = gpio[pin["name"]]
            expected = (pin["header"], pin["pin"], pin["fpga"], pin["line"],
                        mapping["gpio_base"] + pin["line"])
            actual = (entry["header"], entry["pin_num"], entry["fpga_pin"],
                      entry["line"], entry["sysfs_gpio"])
            if actual != expected:
                raise ValueError(f"GPIO configuration mismatch: {pin['name']}")
    config = (root / "t9_programmer/usbip_dap/DAP_config.h").read_text()
    for name, expected in (("PIN_SWCLK", 0), ("PIN_SWDIO", 1), ("PIN_NRST", 2),
                           ("BOOT0_SYSFS_GPIO", 569)):
        match = re.search(r"#define\s+" + name + r"\s+(\d+)", config)
        if not match or int(match[1]) != expected:
            raise ValueError(f"C pin-map mismatch: {name}")


if __name__ == "__main__":
    check(Path(__file__).resolve().parents[1])
    print("Pin map: OK")
