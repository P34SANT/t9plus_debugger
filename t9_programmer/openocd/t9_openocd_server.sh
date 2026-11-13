#!/bin/bash
# ==============================================================================
# t9_openocd_server.sh - Auto-recovering OpenOCD SWD Debug Server on Antminer T9+
# ==============================================================================
# Usage:
#   ./t9_openocd_server.sh [target] [interface]
# Default target: stm32f1x
# Default interface: t9_swd.cfg (linuxgpiod)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${1:-stm32f4x}"
INTERFACE="${2:-$SCRIPT_DIR/t9_swd.cfg}"

# If target is given as simple name without path or .cfg, resolve standard target
if [[ ! -f "$TARGET" ]]; then
    if [[ "$TARGET" != *.cfg ]]; then
        TARGET="target/${TARGET}.cfg"
    fi
fi

echo "=========================================================================="
echo " Starting Antminer T9+ STM32 OpenOCD Server Supervisor"
echo " Interface: $INTERFACE"
echo " Target:    $TARGET"
echo " Ports:     GDB: 3333 | Telnet: 4444 | TCL: 6666 (0.0.0.0)"
echo "=========================================================================="

trap "echo 'Terminating OpenOCD server supervisor...'; exit 0" SIGINT SIGTERM

while true; do
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting OpenOCD..."
    openocd \
        -f "$INTERFACE" \
        -f "$TARGET" \
        -c "reset_config none" \
        -c "cortex_m reset_config sysresetreq" \
        -c "catch {stm32f4x.cpu configure -event reset-start {}}" \
        -c "catch {stm32f4x.cpu configure -event reset-init {}}"
    
    EXIT_CODE=$?
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] OpenOCD stopped (exit code $EXIT_CODE)."
    echo "Target MCU may be disconnected or sleeping. Re-probing in 5s..."
    sleep 5
done
