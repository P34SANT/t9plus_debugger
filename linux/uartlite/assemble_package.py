#!/usr/bin/env python3
"""Verify a module build, then assemble a separately hashed integration package."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assemble(build, destination):
    manifest = json.loads((build / "manifest.json").read_text())
    if (manifest.get("kernel_commit") != "e475cc1c9432587298212570c2af2be66b820c41" or
            manifest.get("kernel_release") != "6.6.0-xilinx-ge475cc1c9432"):
        raise ValueError("Wrong kernel build")
    for name, expected in manifest["files"].items():
        if Path(name).name != name or name in (".", "..") or sha(build / name) != expected:
            raise ValueError("Module build mismatch")
    destination.mkdir(parents=True, exist_ok=False)
    for name in manifest["files"]:
        shutil.copyfile(build / name, destination / name)
    source = Path(__file__).resolve().parent
    for name in ("activate.py", "native_bridge.py", "plink-uartlite.service", "native-bridge.conf", "install.sh"):
        shutil.copyfile(source / name, destination / name)
    manifest["files"] = {p.name: sha(p) for p in sorted(destination.iterdir())}
    (destination / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(sha(destination / "manifest.json"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    assemble(args.build, args.destination)
