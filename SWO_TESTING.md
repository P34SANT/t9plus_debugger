# SWO verification and safety status

## Live testing paused — November 18, 2026

Windows recorded bugcheck `0x0000000A` (`IRQL_NOT_LESS_OR_EQUAL`) at
16:28:21 local time. The event log references
`C:\Windows\Minidump\111826-9515-01.dmp`. The responsible driver has **not**
been established; USB/IP involvement is a hypothesis, not a finding.

Do not automatically attach/detach USB/IP devices, restart the virtual root
hub, restart board services, reload the FPGA, or flash the target. Obtain
explicit approval before any further live tests. Duplicate imports of the
same device were observed before the crash; repeated reconnects are not an
acceptable recovery experiment.

The new SWO capture, serial comparison, and target-register scripts refuse
live access without `--allow-live-hardware`. That flag is not a guarantee of
safety. Older diagnostic scripts are not covered by these guards.

## Historical ABI 1 observations

- STM32F407 firmware rebuilt with zero warnings/errors and flashed/verified.
- Asynchronous trace requires `DBGMCU_CR.TRACE_MODE=00`, not `01`.
- OpenOCD captured 920 raw ITM bytes at 2 Mbps, decoding ten complete SWO
  pulse messages. Configuration requires separate `-traceclk 16000000` and
  `-pin-freq 2000000`, with `-formatter off`.
- The Python capture subsequently received 1,840 bytes / 20 SWO pulses in
  ten seconds. COM6 contained all 20 matching pulse IDs without malformed
  lines.
- Earlier COM10 captures were malformed or empty. A later, explicitly approved
  bounded three-second read received 276 bytes with consecutive clean USART1
  pulse IDs #1399–1404 and no observed crash. This is a smoke test, **not**
  sustained-load or concurrent-path qualification.
- A direct TCP connection to UART1 returned readable pulse messages after
  the dedicated polling-worker change. This does not verify the Windows CDC
  path or prove the UART bridge is lossless.

## Changes requiring further review

The hardening work now introduces ABI 2, documented in [HARDENING.md](HARDENING.md).
It remains offline-only and has not been loaded onto the board. Earlier
measurements above describe ABI 1, not the new RTL/software.

- CMSIS-DAP SWO buffer-size and status-count fields are 32-bit; extended
  status uses a one-byte field mask; overflow is bit 7.
- Unsupported streaming-endpoint transport is rejected; zero baud does not
  divide by zero.
- T10 remains input after capture stops. Never clamp it while connected to
  the target's PB3 output. Local default pin configuration was changed to
  input; deployment of that configuration still needs review.
- The capture utility decodes ITM source packets instead of interpreting
  packet headers as text. Its SWD setup requests debug/system power-up
  without rewriting the firmware's trace configuration.
- Full-sized DAP requests match OpenOCD. The earlier claim that padding
  alone fixed capture was not isolated: debug-domain power-up was also
  necessary. Short-request behavior has not been established.
- UART1 polling uses aligned word access and a dedicated worker; ABI 2 prefers
  a verified Linux UARTLite TTY when available. Sustained CDC verification,
  concurrent traffic and IRQ/device-tree qualification remain pending.

## Safe offline checks

```powershell
python -m unittest discover -s tests -p test_swo_tools.py -v
git diff --check
```

These unit tests use mocked USB/serial access. They do not attach devices,
open COM ports, or modify the target. Live throughput, overflow recovery,
driver stability, and simultaneous CDC traffic are **not** covered.
