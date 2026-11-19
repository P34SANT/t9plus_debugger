#!/bin/bash
# Finish a prepared full-core build without discarding any completed work.
set -euo pipefail
[[ $# == 2 ]] || { echo "Usage: $0 <exact-linux-source> <prepared-output>" >&2; exit 2; }
SOURCE=$(realpath "$1")
OUTPUT=$(realpath "$2")
COMMIT=e475cc1c9432587298212570c2af2be66b820c41
export ARCH=arm
export CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-4}
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$COMMIT" ]]
[[ -z $(git -C "$SOURCE" status --porcelain --untracked-files=no) ]]
test -s "$OUTPUT/kernel/vmlinux.symvers"
test -s "$OUTPUT/kernel/vmlinux"
test -s "$OUTPUT/running.config"
grep -qx 'CONFIG_SERIAL_UARTLITE=m' "$OUTPUT/kernel/.config"
RELEASE=$(make -s -C "$SOURCE" O="$OUTPUT/kernel" kernelrelease)
[[ "$RELEASE" == 6.6.0-xilinx-ge475cc1c9432 ]]
# Full vmlinux supplies CRCs; modules_prepare additionally creates module.lds.
make -C "$SOURCE" O="$OUTPUT/kernel" modules_prepare
cp "$OUTPUT/kernel/vmlinux.symvers" "$OUTPUT/kernel/Module.symvers"
cp "$SOURCE/drivers/tty/serial/uartlite.c" "$OUTPUT/module/uartlite.c"
printf 'obj-m += uartlite.o\n' > "$OUTPUT/module/Makefile"
make -C "$SOURCE" O="$OUTPUT/kernel" M="$OUTPUT/module" -j"$JOBS" modules
cp "$OUTPUT/module/uartlite.ko" "$OUTPUT/package/"
cp "$OUTPUT/running.config" "$OUTPUT/package/"
cp "$OUTPUT/kernel/.config" "$OUTPUT/package/build.config"
cp "$OUTPUT/kernel/Module.symvers" "$OUTPUT/package/"
modinfo "$OUTPUT/package/uartlite.ko"
python3 - "$OUTPUT/package" "$COMMIT" "$RELEASE" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(root.iterdir()) if p.is_file() and p.name != "manifest.json"}
manifest = {"schema": 1, "kernel_commit": sys.argv[2],
            "kernel_release": sys.argv[3], "files": files}
(root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
PY
