#!/bin/bash
# First installation only. No boot partition changes, forced modules or FPGA load.
set -euo pipefail
[[ $EUID == 0 && $# == 2 && $2 == --allow-live-hardware ]] || {
    echo "Usage: sudo bash install.sh <verified-package> --allow-live-hardware" >&2
    exit 2
}
SOURCE=$(realpath "$1")
python3 "$SOURCE/activate.py" verify
[[ $(uname -r) == 6.6.0-xilinx-ge475cc1c9432 ]]
UNIT=/etc/systemd/system/plink-uartlite.service
DROPIN=/etc/systemd/system/t9-uart-bridge.service.d/50-plink-native-uart.conf
CURRENT=/opt/plink/uartlite-current
[[ ! -e "$UNIT" && ! -e "$DROPIN" && ! -e "$CURRENT" && ! -L "$CURRENT" ]] || {
    echo "Existing native integration found; refusing to overwrite it." >&2
    exit 1
}
ID=$(sha256sum "$SOURCE/manifest.json" | cut -c1-16)
DEST="/opt/plink/host-drivers/uartlite/$ID"
if [[ -e "$DEST" ]]; then
    python3 "$DEST/activate.py" verify
else
    mkdir -p "$DEST"
    cp -a "$SOURCE/." "$DEST/"
    chown -R root:root "$DEST"
    chmod -R go-w "$DEST"
fi
python3 "$DEST/activate.py" verify
exec 9>/run/lock/plink-native-uart-install.lock
flock -n 9 || { echo "Another UART driver install is active" >&2; exit 1; }
systemctl stop t9-uart-bridge
exec 8>/run/lock/plink-uart0.lock
exec 7>/run/lock/plink-uart1.lock
flock -n 8
flock -n 7
created_link=false
created_unit=false
created_dropin=false
rollback() {
    trap - ERR
    systemctl stop t9-uart-bridge || true
    systemctl stop plink-uartlite || true
    python3 "$DEST/activate.py" stop || true
    systemctl disable plink-uartlite || true
    $created_dropin && rm -f "$DROPIN"
    $created_unit && rm -f "$UNIT"
    $created_link && rm -f "$CURRENT"
    systemctl daemon-reload
    flock -u 8
    flock -u 7
    if [[ -e /sys/module/uartlite ]]; then
        echo "Driver cleanup incomplete; UART bridge left stopped to prevent two consumers." >&2
    else
        systemctl start t9-uart-bridge || true
    fi
    echo "Native UART installation failed. Boot files were not modified." >&2
    exit 1
}
trap rollback ERR
ln -s "$DEST" "$CURRENT"
created_link=true
install -m 644 "$DEST/plink-uartlite.service" "$UNIT"
created_unit=true
mkdir -p "$(dirname "$DROPIN")"
install -m 644 "$DEST/native-bridge.conf" "$DROPIN"
created_dropin=true
systemctl daemon-reload
systemctl start plink-uartlite
test -c /dev/ttyUL0
flock -u 8
flock -u 7
systemctl start t9-uart-bridge
systemctl is-active --quiet plink-uartlite t9-uart-bridge
systemctl enable plink-uartlite
trap - ERR
echo "Native UART driver and bridge started. Run bounded traffic/counter checks."
echo "No boot kernel, boot DTB, FPGA image or Windows attachment was changed."
