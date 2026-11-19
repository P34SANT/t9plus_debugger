# Native UARTLite driver for Plink

This is a separately versioned Linux integration package. It does not replace
the FPGA bitstream or modify the immutable ABI 2 probe release.

The observed board runs Xilinx Linux
`6.6.0-xilinx-ge475cc1c9432`, with `CONFIG_SERIAL_UARTLITE` disabled and only
unrelated Linux 5.15 headers installed. Do not use those headers, force module
loading, or suppress symbol-version checks.

## Build

`build_module.sh` uses:

- the exact Xilinx source commit `e475cc1c9432587298212570c2af2be66b820c41`;
- the running board's exported `/proc/config.gz`;
- a full `vmlinux` build to generate exported symbol CRCs;
- the unmodified upstream `drivers/tty/serial/uartlite.c`;
- a separate external-module build with normal version checks.

The script requires an already checked-out source directory, an exported kernel
configuration, an output directory, GNU make, an ARM GNU cross toolchain and the
usual kernel build dependencies. It refuses to reuse a nonempty build directory.
Nothing is installed on the board by the build script.

```sh
bash build_module.sh <linux-source> <running.config.gz> <output>
```

The first choice is a normally loaded module for the existing kernel—not a boot
kernel replacement. A rejected module is a stop condition, not permission to
force-load it.

## Hardware requirements

The ABI 2 FPGA routes AXI UARTLite at `0x42c00000` to `IRQ_F2P[0]`, GIC interrupt
61 (SPI index 29). The upstream driver requests a **rising-edge** interrupt.
The generated overlay must therefore use `<0 29 1>`, not an assumed level IRQ.
It also needs `current-speed = <115200>` and the named `s_axi_aclk` clock.

The interrupt-controller phandle must be read from the actual live device tree.
The board's AXI bus uses one address cell and one size cell. Do not copy numeric
phandles blindly to another device tree.

The verified FPGA loader owns FCLK0. The overlay describes its established
100 MHz rate through a local fixed-clock provider and checks the live SLCR
configuration before binding. This is intentional: allowing the UART driver's
runtime PM to gate the shared PS FCLK0 could stop **all** FPGA engines when a TTY
closes or the UART driver unloads. The native UART must not own that shared gate.

The raw MMIO bridge must be stopped before probing the native driver. Never run
the kernel driver and a `/dev/mem` UART consumer concurrently.

The factory base tree has no `__symbols__` node. The overlay uses absolute
target paths and only local clock-label references; compile it without `dtc -@`
so no exported overlay symbol table is added. The kernel-reported configfs
overlay status must be `applied`, not merely a successful dtbo write.

## Recovery boundary

Before installation, preserve and checksum `BOOT.BIN`, `uImage`, `boot.scr`,
both DTBs, the initramfs and `uEnv.txt`, plus the running configuration/device
tree. Test a temporary module/overlay before enabling a persistent boot unit.
The original boot files must remain unchanged when the hot-load path works.

Driver installation and service ordering must ensure the FPGA is verified and
loaded **before** the UART node is instantiated, and the UART consumer/driver
is stopped **before** any FPGA reload. A persistent native-mode bridge must
fail closed rather than silently resume the lossy polling fallback.

## Assemble and install

On the build host:

```sh
python3 assemble_package.py <output>/package <new-native-package>
```

Copy that directory to the board. With no flash worker or other UART owner
running, inspect the read-only plan before installing:

```sh
python3 <new-native-package>/activate.py verify
python3 <new-native-package>/activate.py plan
sudo bash <new-native-package>/install.sh <new-native-package> --allow-live-hardware
```

Board dependencies are Python 3, `dtc`, `kmod`, `flock` and systemd. The installer
is deliberately **first-install only**; it refuses an existing native integration.
It installs under `/opt/plink/host-drivers/uartlite/<manifest-hash-prefix>` and
links `/opt/plink/uartlite-current`. It does not edit `/boot` or the matched probe
release. Its systemd order is:

1. `t9-fpga`: verify and load the matched ABI 2 release.
2. `plink-uartlite`: check the clock/core identities, load the module normally,
   apply the overlay and verify `/dev/ttyUL0`.
3. `t9-uart-bridge`: use the required native TTY, never fall back to MMIO.

The wrapper reports `kernel_tty_irq` and `TIOCGICOUNT` counters through the
existing metrics endpoint. Kernel counters are cumulative since driver binding;
compare deltas across a test. A missing/unavailable counter is not zero.

Useful checks on the board:

```sh
systemctl is-active t9-fpga plink-uartlite t9-uart-bridge t9-cmsis-dap t9-io-daemon
cat /run/plink/uartlite-native.json
cat /sys/kernel/config/device-tree/overlays/plink-uartlite/status
grep uartlite /proc/interrupts
python3 /opt/plink/uartlite-current/activate.py verify
```

## Bounded live qualification

`qualify_serial.py` is a Windows-host test for the current STM32 pulse firmware,
not a generic throughput generator. It requires `pyserial`; concurrent SWO also
uses the dependencies of `scripts/swo_dap_test.py`. COM6 was the CH340 reference,
COM10 the Plink UART1, and COM9 the unexercised UART0. Verify these assignments
before testing; do not assume they survive every Windows installation.

```powershell
python linux/uartlite/qualify_serial.py --allow-live-hardware `
  --seconds 180 --warmup 5 --ports COM6 COM10 --swo `
  --output C:\path\to\new-evidence-directory
```

The test discards old transport backlog, captures at 115200 baud, compares
consecutive pulse IDs and their ticks, and saves raw serial/SWO plus JSON metrics.
Capture is limited to 300 seconds and 8 MiB per serial stream. Fresh measured
UART counters and, when requested, zero SWO loss/framing counters are required.
SWO uses the existing 2 Mbaud target configuration; it wakes the debug domain
without flashing firmware. Do not run another debugger or serial consumer
against the same interfaces during qualification.

Sparse ~0.5-second pulses establish correctness only at the observed load.
They do **not** establish full-rate dual-UART performance or external SWD timing.

## Stop, restart and recovery

For a native-driver lifecycle check (no FPGA reload):

```sh
sudo systemctl stop t9-uart-bridge plink-uartlite
test ! -e /sys/module/uartlite
test ! -e /run/plink/uartlite-native.json
sudo systemctl start t9-uart-bridge
```

Starting the bridge brings its native-driver dependency back automatically.
If activation fails, inspect `journalctl -b -u plink-uartlite` and the live DT,
kernel identity and FPGA-ready marker. Do not force the module or bypass the
bridge override. Keep the bridge stopped if driver cleanup fails.

To remove **this integration only**, stop the bridge and native unit first,
then confirm the module and overlay really disappeared. Only after successful
cleanup:

```sh
sudo systemctl disable plink-uartlite
sudo rm /etc/systemd/system/t9-uart-bridge.service.d/50-plink-native-uart.conf
sudo rm /etc/systemd/system/plink-uartlite.service
sudo rm /opt/plink/uartlite-current
sudo systemctl daemon-reload
```

Leave the immutable driver directory and backups preserved. Do not restart the
original polling bridge as though it were qualified: that backend showed losses.
The boot kernel/DTB need no restoration because this installation never changed
them. For a planned board reboot, first exit the known tray instance and detach
only `usbip://192.168.2.99:3240/1-1`; reattach once after all board services pass.
Never reset a USB hub as routine recovery.

Live provenance and limitations are in
[`docs/native-uart-live-20261119.md`](../../docs/native-uart-live-20261119.md).
