# Plink native UART1 live validation — 2026-11-19

## Outcome and scope

The original live UART1 MMIO-polling path lost/corrupted incoming STM32 pulse
lines. The board's running kernel had no UARTLite driver enabled. UART1 now
uses the unmodified upstream UARTLite driver with a rising-edge interrupt and
`/dev/ttyUL0`, while preserving CMSIS-DAP, COM9/COM10 and the ABI 2 FPGA release.
No boot kernel replacement was needed.

This validates the observed sparse STM32 pulse workload, **not** maximum UART
throughput. UART0 had no comparable input. External SWD/cable timing still
requires instrumentation. The earlier Windows BSOD remains unattributed.

## Deployed identities

| Item | Identity |
|---|---|
| Board | `xilinx@192.168.2.99`, Zynq/T9+ |
| Probe release | `/opt/plink/releases/2df243f00215eab8` |
| Probe manifest SHA-256 | `2df243f00215eab8654283039aedc349fd2d3b3f0bd3b17818f1ed78f3ed1ba2` |
| Running kernel | `6.6.0-xilinx-ge475cc1c9432` |
| Exact upstream kernel commit | `e475cc1c9432587298212570c2af2be66b820c41` |
| Native package directory | `/opt/plink/host-drivers/uartlite/a158979e85be12d7` |
| Native package manifest SHA-256 | `a158979e85be12d73dfe7b98db8f1b032834fbfc6304a704fac8fc8851af717c` |
| Module SHA-256 | `f860dcb3377a76e324eee1540d0e5cf85591816665f22da4e8850e9466c0ec85` |

The native package manifest is preserved in
[`native-uart-package-manifest.json`](native-uart-package-manifest.json).
This integration is separate from the unchanged ABI 2 release source manifest.
Kernel trees, modules, backup archives and credentials are not added to Git.

The module was built with GNU ARM GCC 15.2 against the exact source and exported
running configuration; the original kernel compiler was GCC 11.2. A full
`vmlinux` build generated symbol CRCs. `modules_prepare` also generated the
module linker script. The running kernel accepted normal `insmod` and `rmmod`
with `modversions`; no force option or CRC bypass was used.

## Board-specific integration

- UARTLite: `0x42c00000`, 115200 8N1, 100 MHz FPGA clock.
- GIC SPI index 29 / hardware interrupt 61, rising-edge (`<0 29 1>`).
- Live parent: `/axi`; the interrupt-controller phandle is discovered, not copied.
- A local fixed-clock provider describes the already established FCLK0 rate,
  without giving UART runtime PM ownership of the shared hardware clock gate.
- Activation checks the FCLK0 configuration and SWD/SWO ABI 2 core identities.
- The factory base DT has no `__symbols__`. The initial `dtc -@` attempt was
  rejected and the installer restored the original bridge safely. The successful
  package compiles without exported symbols and checks configfs status `applied`.
- Systemd starts verified FPGA → native module/overlay → UART bridge; stop order
  is reversed. The bridge override requires the native TTY and fails closed.

## Completed live checks

All captures discarded five seconds of transport backlog first. The initial
clean test discarded 155,821 queued COM10 bytes from historical traffic.

| Check | Result |
|---|---|
| Normal module load/unload | Accepted by the running kernel |
| Native module/overlay + bridge stop/start | Clean unload; dependency-driven reload passed |
| Native serial, 10 seconds | COM6 and COM10: 980 identical bytes, 20 consecutive pulses each |
| Concurrent SWO 60 s / serial 62 s | 11,900 SWO bytes, 119 consecutive SWO pulses; each serial stream 6,027 identical bytes / 123 pulses |
| Pulse correlation in 60 s test | All 119 SWO IDs occurred in both serial captures |
| Kernel error deltas | Zero hardware overruns, buffer overruns, framing or parity errors |
| SWO telemetry after 60 s test | Zero software drops, hardware drops and framing errors |
| Post-startup concurrent SWO 180 s / serial 182 s | 32,936 SWO bytes / 358 consecutive messages; each serial stream 16,290 identical bytes / 362 consecutive pulses |
| Post-startup correlation and telemetry | All 358 SWO IDs matched both serial streams; serial ticks matched; zero UART error deltas and zero SWO drop/framing counters |

The 60-second serial IDs were `92325..92447`; SWO IDs were `92325..92443`.
SWO was configured for 2 Mbaud but carried sparse messages, not continuous
2 Mbaud payload. Likewise, 115200 baud is a line setting, not measured throughput.

The post-startup test began at `2026-11-19T06:51:50.586713+00:00` host UTC
and took 187.141 seconds including warmup. Serial IDs were `440..801`; SWO
IDs were `441..798`. Target pulse numbering had restarted before this capture;
the test did not reset or reflash the target. No gaps appeared during capture.
The stricter qualifier required fresh telemetry, compared serial ticks and
rejected missing counters rather than treating them as zero.

### Startup persistence observed

On resuming at approximately 10:19 host time, SSH/API connections initially
reset. The board then reported about one minute of uptime and boot ID
`0bf45cf4-8707-4efb-86ec-adc465396de5`. No reboot command was issued during
that resumption; the cause of this restart is not established.

The current boot journal independently showed:

1. `t9-fpga` verified ABI 2 and FCLK0 at 100 MHz.
2. `plink-uartlite` loaded successfully.
3. The bridge opened `/dev/ttyPS1` and `/dev/ttyUL0`.

All five services were enabled and active, configfs reported `applied`, the
package verification passed and the UARTLite interrupt count increased.
This establishes automatic native-driver startup on this observed boot, not
qualification of repeated reboots or attribution of the unexpected restart.
Windows USB/IP was initially detached; one explicit attach of pod `1-1`
restored COM9/COM10. No hub reset or reconnect loop was used.

After qualification, `DAP_Disconnect` succeeded and released the debug pins.
The tray was launched again, with the single matching pod still attached.
The Windows boot time remained `2026-11-18 16:28:12`; no new Windows reboot
was observed during these checks.

### Offline regressions rerun

- Python: 30 passed on Windows, six Linux-only tests skipped there.
- All six Linux TTY/ownership tests passed separately in WSL.
- Eight native integration/qualification tests passed, including counter
  layout, tamper/wrong-kernel rejection and stale/missing/error telemetry.
- Go tray tests passed.
- C transport, real DAP-handler and SWO-ring tests rebuilt and passed.
- Python compilation, shell syntax and unchanged ABI 2 manifest verification passed.

## Evidence and checksums

Raw evidence is preserved locally under
`C:\Users\Peasant\AppData\Local\Temp\opencode\plink-live-20261119\`.
Board journal calendar dates are in October 2022; correlate by host timestamps,
boot ID, monotonic durations and pulse IDs, not board wall-clock dates.

| Evidence | SHA-256 |
|---|---|
| `native-serial-10s/report.json` | `41a9658eada654de1a5ce29b92280b60fba06c58f6ecd0a8fb42ab74ea04e15a` |
| Both 10 s serial binaries | `4944166292d4f834517e1fac53c19f09f2219bc30de00b102dad83423dea5550` |
| `native-concurrent-60s/report.json` | `d6eea9316ba6bc0c35f7a2e8f50e411c6c854fd69ce3149d0eb0f1e4028c564b` |
| Both 62 s serial binaries | `82e1ebf14937fe47d9fefed018cf6531248178b904a996628532672288b6f50c` |
| `native-concurrent-60s/swo.bin` | `c75368c2507dbe53d56e717589506285375c2cd1d4216cda8a6a3153dbab4b48` |
| `native-postboot-concurrent-180s/report.json` | `33eebeab1e7d6aa73f5090611fb3264132c0f536f25bb04d37a284359cb88f08` |
| Both 182 s serial binaries | `17aee04582f9f9da89695e180f1337306d674bb6aaf9eb8e506306e540133cc4` |
| `native-postboot-concurrent-180s/swo.bin` | `5432ff6f47af0f5ced9e4fde37c8395ec5bd5135a699aad99f9e1faaa4a0f9a4` |
| Post-startup `startup-journal.txt` | `2d7c68a01a229dfd3a9045eef410d1388ade387189a04f351a36b94c44edbdfc` |
| Post-startup `board-final-readonly.txt` | `67a372b66029c749a168879a74be421b47e2ce1183d137267c62d2c430fbd8ae` |

## Boot preservation and recovery

The original boot archive was preserved locally and on the board:
SHA-256 `68e7f1eaea09a2c1cc975d10e9b2096bac10d474e530dfe7a3e1840583f35244`.
The prior installation archive SHA-256 is
`c2b92b0ee3a092df96076c52eed6e2f24b803dfa52a29fe43e276400c9331b85`.

The following boot files were checksummed again after the observed restart and
remained unchanged:

| File | SHA-256 |
|---|---|
| `BOOT.BIN` | `4004015bba6ff3d64c7626d3be2e24bf4486fe4bad1261be7b880ca6ef693e1c` |
| `uImage` | `ff538c847b49a1e67ddbd7b283a8a6cd239815fb3a05aeb0e833cd4898cf8ed8` |
| `boot.scr` | `7013e14923fbe461c4ecd745214fcafcb0ffaddb10865f9135cd8624637359f4` |
| Both active DTBs | `f18245a301d9421b77000ff8de5e607efff47233a1326ab1fc4020d5786cc244` |
| `uramdisk.image.gz` | `247130ce8d8ca18b034b4c527dc000de8dc1d94d488aad72d5b4eab24b425372` |

No target firmware flash or BOOT0 drive was performed. STM32 PB3 remains SWO
on J3.6 / FPGA T10. Pre-existing FAT/unclean-shutdown warnings were not repaired.
See [`linux/uartlite/README.md`](../linux/uartlite/README.md) for installation,
diagnosis and exact integration-removal instructions.
