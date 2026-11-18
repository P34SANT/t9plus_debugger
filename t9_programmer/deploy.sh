#!/bin/bash
set -euo pipefail
[[ ${1:-} == --allow-live-hardware ]] || {
    echo "Deployment/reconfiguration requires explicit --allow-live-hardware approval" >&2
    exit 2
}
[[ $EUID == 0 ]] || { echo "Run with sudo" >&2; exit 2; }
exec 9>/run/lock/plink-deploy.lock
flock -n 9 || { echo "Another deployment is active" >&2; exit 1; }
python3 scripts/release_manifest.py verify --root "$PWD" --output release
ID=$(sha256sum release/manifest.json | cut -c1-16)
DEST="/opt/plink/releases/$ID"
mkdir -p /opt/plink/releases /run/plink /etc/plink /var/lib/plink/uploads
chmod 700 /var/lib/plink/uploads
if [[ -e "$DEST" ]]; then
    python3 "$DEST/scripts/release_manifest.py" verify --root "$DEST" --output "$DEST/release"
    [[ -x "$DEST/t9_programmer/usbip_dap/t9-cmsis-dap" ]] || {
        echo "Existing release is incomplete; inspect it before retrying." >&2
        exit 1
    }
else
    mkdir "$DEST"
    cp -a scripts t9_programmer vivado constraints plink-tray hardware release "$DEST/"
    chown -R root:root "$DEST"
    chmod -R go-w "$DEST"
    # Build only a newly staged release; never rewrite an installed executable.
    make -C "$DEST/t9_programmer/usbip_dap" clean all
    chmod 755 "$DEST/t9_programmer/bin/t9-io" "$DEST/t9_programmer/bin/t9-reset"
fi
# Verify before touching any running service.
python3 "$DEST/scripts/release_manifest.py" verify --root "$DEST" --output "$DEST/release"
if [[ ! -f /etc/plink/api.token ]]; then
    (umask 077; python3 -c 'import secrets; print(secrets.token_hex(32))' > /etc/plink/api.token)
fi
PREVIOUS=$(readlink -f /opt/plink/current || true)
SERVICES=(t9-cmsis-dap t9-uart-bridge t9-io-daemon t9-openocd plink-stlink t9-programmer)
for service in "${SERVICES[@]}"; do
    if [[ $(systemctl show "$service" -p LoadState --value) != not-found ]]; then
        systemctl stop "$service"
        case "$service" in t9-openocd|plink-stlink|t9-programmer)
            systemctl disable "$service"
        esac
    fi
done
for service in "${SERVICES[@]}"; do
    if systemctl is-active --quiet "$service"; then exit 1; fi
done
exec 8>/run/lock/plink-pod1.lock
exec 7>/run/lock/plink-pod2.lock
exec 6>/run/lock/plink-uart0.lock
exec 5>/run/lock/plink-uart1.lock
flock -n 8 && flock -n 7 && flock -n 6 && flock -n 5 || {
    echo "A non-service operation still owns hardware. Services left stopped." >&2
    exit 1
}
switch_release() {
    [[ ! -e /opt/plink/current || -L /opt/plink/current ]] || {
        echo "Refusing to overwrite a non-symlink /opt/plink/current" >&2; return 1;
    }
    ln -s "$1" /opt/plink/.current.next
    mv -Tf /opt/plink/.current.next /opt/plink/current
}
rollback() {
    trap - ERR
    systemctl stop t9-cmsis-dap t9-uart-bridge t9-io-daemon || true
    rm -f /run/plink/fpga-ready
    if [[ -n "$PREVIOUS" && -f "$PREVIOUS/release/manifest.json" ]]; then
        echo "Restoring previous verified release; clients remain stopped pending inspection." >&2
        switch_release "$PREVIOUS"
        systemctl restart t9-fpga || true
    fi
    echo "Deployment failed. Do not reattach USB/IP until the journal has been inspected." >&2
    exit 1
}
trap rollback ERR
switch_release "$DEST"
for service in t9-fpga t9-cmsis-dap t9-uart-bridge t9-io-daemon; do
    install -m 644 "$DEST/t9_programmer/systemd/$service.service" "/etc/systemd/system/$service.service"
done
systemctl daemon-reload
systemctl restart t9-fpga
[[ $(cat /run/plink/fpga-ready) == plink-abi-2 ]]
flock -u 8; flock -u 7; flock -u 6; flock -u 5
systemctl start t9-uart-bridge
systemctl start t9-io-daemon
systemctl start t9-cmsis-dap
systemctl is-active --quiet t9-fpga t9-uart-bridge t9-io-daemon t9-cmsis-dap
systemctl enable t9-fpga t9-cmsis-dap t9-uart-bridge t9-io-daemon
ln -sfn /opt/plink/current/t9_programmer/bin/t9-io /usr/local/bin/t9-io
ln -sfn /opt/plink/current/t9_programmer/bin/t9-reset /usr/local/bin/t9-reset
trap - ERR
echo "Deployment complete. Windows devices were NOT reattached."
