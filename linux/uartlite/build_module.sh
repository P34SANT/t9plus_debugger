#!/bin/bash
set -euo pipefail
[[ $# == 3 ]] || {
    echo "Usage: $0 <exact-linux-source> <running.config.gz> <new-output>" >&2
    exit 2
}
SOURCE=$(realpath "$1")
CONFIG=$(realpath "$2")
OUTPUT=$(realpath -m "$3")
COMMIT=e475cc1c9432587298212570c2af2be66b820c41
EXPECTED_RELEASE=6.6.0-xilinx-ge475cc1c9432
CROSS_COMPILE=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-4}
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$COMMIT" ]] || {
    echo "Kernel source revision does not match the running board" >&2; exit 1;
}
[[ -z $(git -C "$SOURCE" status --porcelain --untracked-files=no) ]] || {
    echo "Refusing modified kernel sources" >&2; exit 1;
}
[[ ! -e "$OUTPUT" ]] || {
    echo "Output already exists; choose a new directory" >&2; exit 1;
}
mkdir -p "$OUTPUT/kernel" "$OUTPUT/module" "$OUTPUT/package"
gzip -dc "$CONFIG" > "$OUTPUT/running.config"
cp "$OUTPUT/running.config" "$OUTPUT/kernel/.config"
"$SOURCE/scripts/config" --file "$OUTPUT/kernel/.config" \
    --module SERIAL_UARTLITE --set-val SERIAL_UARTLITE_NR_UARTS 4
export ARCH=arm CROSS_COMPILE
make -C "$SOURCE" O="$OUTPUT/kernel" olddefconfig
RELEASE=$(make -s -C "$SOURCE" O="$OUTPUT/kernel" kernelrelease)
[[ "$RELEASE" == "$EXPECTED_RELEASE" ]] || {
    echo "Unexpected module kernel release: $RELEASE" >&2; exit 1;
}
# A full core build is deliberate: modules_prepare alone has no symbol CRCs.
make -C "$SOURCE" O="$OUTPUT/kernel" -j"$JOBS" vmlinux
bash "$(dirname "$(realpath "$0")")/finish_module.sh" "$SOURCE" "$OUTPUT"
