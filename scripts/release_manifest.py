#!/usr/bin/env python3
"""Package and verify a matched FPGA/software release; never touches devices."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from bit_to_pcap import bit_to_pcap

ABI = 2
EXTENSIONS = {".v", ".xdc", ".tcl", ".c", ".h", ".inc", ".py", ".sh",
              ".service", ".json", ".go", ".dtsi", ".html", ".css", ".js"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def source_hashes(root):
    result = {}
    for directory in ("vivado", "constraints", "scripts", "t9_programmer", "plink-tray", "hardware"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and (path.suffix in EXTENSIONS or
                                   path.name in ("Makefile", "go.mod", "go.sum") or
                                   path.parent == root / "t9_programmer" / "bin"):
                # Text checkout line endings differ between Windows and Linux.
                result[path.relative_to(root).as_posix()] = sha(path.read_bytes().replace(b"\r\n", b"\n"))
    return result


def verify(directory, root=None):
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("abi") != ABI or manifest.get("clock_hz") != 100000000:
        raise ValueError("Unsupported release ABI/clock")
    expected = {"t9_programmer.bit", "t9_programmer.hwh", "t9_programmer_pcap.bin"}
    if set(manifest.get("artifacts", {})) != expected:
        raise ValueError("Incomplete artifact manifest")
    for name, info in manifest["artifacts"].items():
        data = (directory / name).read_bytes()
        if len(data) != info["size"] or sha(data) != info["sha256"]:
            raise ValueError(f"Artifact mismatch: {name}")
    if bit_to_pcap((directory / "t9_programmer.bit").read_bytes()) != \
            (directory / "t9_programmer_pcap.bin").read_bytes():
        raise ValueError("PCAP does not match bitstream")
    if root is not None and source_hashes(root) != manifest["sources"]:
        raise ValueError("Sources changed since the release build")
    return manifest


def package(root, bit, hwh, snapshot, output):
    sources = source_hashes(root)
    if sources != json.loads(snapshot.read_text()):
        raise ValueError("Sources changed during Vivado build; rebuild before packaging")
    hwh_data = hwh.read_bytes()
    for required in (b"axi_swd_0", b"axi_swd_1", b"axi_swo_0", b"43C20000"):
        if required not in hwh_data:
            raise ValueError(f"Missing hardware block: {required!r}")
    if output.exists():
        raise ValueError("Release directory already exists; choose a new immutable directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        directory = Path(temp)
        artifacts = {
            "t9_programmer.bit": bit.read_bytes(),
            "t9_programmer.hwh": hwh_data,
            "t9_programmer_pcap.bin": bit_to_pcap(bit.read_bytes()),
        }
        manifest = {"abi": ABI, "clock_hz": 100000000, "sources": sources,
                    "artifacts": {name: {"size": len(data), "sha256": sha(data)}
                                  for name, data in artifacts.items()}}
        for name, data in artifacts.items():
            (directory / name).write_bytes(data)
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        verify(directory, root)
        shutil.copytree(directory, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["snapshot", "package", "verify"])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bit", type=Path)
    parser.add_argument("--hwh", type=Path)
    parser.add_argument("--snapshot", type=Path)
    args = parser.parse_args()
    if args.action == "snapshot":
        args.output.write_text(json.dumps(source_hashes(args.root), indent=2))
    elif args.action == "package":
        if not all((args.bit, args.hwh, args.snapshot)):
            parser.error("package requires --bit, --hwh and --snapshot")
        package(args.root, args.bit, args.hwh, args.snapshot, args.output)
    else:
        verify(args.output, args.root)
    print(f"{args.action}: OK")


if __name__ == "__main__":
    main()
