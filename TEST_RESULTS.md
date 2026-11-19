# Offline validation — 2026-11-18

**Web update — 2026-11-19:** the interface now uses standard Pinout, Serial,
Firmware and Status tabs, restores horizontal 2×12 connector diagrams and
requires no API token. Desktop/mobile browser tests and token-free API tests
passed. See [`docs/web-ui-20261119.md`](docs/web-ui-20261119.md).

**Later live validation:** ABI 2 was subsequently deployed, and UART1 was moved
to an IRQ-backed native driver. See
[`docs/native-uart-live-20261119.md`](docs/native-uart-live-20261119.md)
for deployment, startup persistence, traffic results and remaining limits.
The offline-only statements below describe the earlier hardening pass.

## Result

The final frozen-source Vivado build completed and packaged a matched ABI 2
release at `releases/20261118-233215`. A separate manifest verification passed.
This local release directory is ignored by Git; its provenance manifest is
recorded at `docs/abi2-release-manifest.json`. Repository-root `.bit` and `.hwh`
copies come from this build. The old root PCAP remains a legacy artifact and
is not an input to deployment.

## Checks passed

| Check | Result |
|---|---|
| Python regression suite on Windows | 22 passed; six Linux-specific tests skipped |
| Linux/WSL TTY, ownership and subprocess tests | All six passed |
| Go tray tests | Both passed; executables rebuilt, not launched |
| C transport tests | Passed: cancellation lifecycle, CDC retention, queues, parser bounds |
| Real C DAP handlers | Passed: malformed writes, field widths, zero baud, response bounds |
| C SWO software ring | Passed: wrap, capacity, drops and framing telemetry |
| ARM Cortex-A9 cross-compilation | Passed with `-Wall -Wextra -pthread`, no warnings |
| JavaScript syntax | Passed |
| Real-browser UI smoke tests | Passed at 1440 px desktop and 390 px mobile widths |
| UI telemetry states | Unknown, fresh and stale states checked |
| UI control protection | Read-only authentication gate checked against local mock server |
| RTL simulation | Passed FIFO/AXI/UART tests and 48 SWD turnaround/data-phase/ACK combinations |
| Standalone SWO synthesis | Eight RAMB36 primitives inferred |
| Full FPGA synthesis/implementation/bitstream | Passed |
| Routed DRC report | No violations |
| Internal timing | Setup WNS **+0.632 ns**, hold WHS **+0.042 ns** |
| Canonical pin map and deployment script syntax | Passed |
| Release source/artifact/PCAP verification | Passed |

The internal clock is constrained to 100 MHz. There are no unconstrained
internal endpoints. The placed design uses 1,813 LUTs, 2,039 registers and eight
RAMB36 blocks.

## Limits

The methodology report retains ten `TIMING-18` warnings: five missing external
input delays and five missing external output delays. These require real
target/cable timing data; they were not suppressed or replaced with invented
values. Internal timing closure is not external-interface qualification.

No ABI 2 deployment, target flash, board restart, USB/IP attach/detach or hub
reset was performed during this hardening pass. Sustained traffic, IRQ/device
tree behavior, full target compatibility and Windows driver stability remain
unqualified. The earlier Windows crash has no established driver attribution.

The screenshot originally recorded here was a localhost mock preview.
`docs/plink-overview.png` has since been replaced with the redesigned live
interface captured on 2026-11-19; it is not the earlier offline-build evidence.
