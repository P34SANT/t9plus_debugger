# Plink hardening audit

Implementation status is not live qualification. ABI 2 has not been deployed
or exercised on a target during this hardening pass.

## Addressed in source

| Defect | Correction / regression evidence |
|---|---|
| USB/IP unlink double completion | Cancelled pending SUBMIT is removed; only RET_UNLINK is returned. Socketpair test checks no second response. |
| Duplicate pod ownership | Exclusive per-pod advisory locks, shared by C server and Python controls/flash workers. |
| CDC short-read truncation | Per-channel bounded RX rings preserve unread bytes. |
| Partial serial/TCP write loss | Retain unsent TCP bytes; loop through partial TTY writes with deadlines. |
| Unbounded client buffering | Slow clients close at a bounded threshold; MMIO delivery callbacks are coalesced and buffered up to 64 KiB. |
| DAP reply overwrite | Eight bounded reply slots; endpoint-specific pending reads. |
| Malformed DAP writes causing target effects | Whole-command validation before dispatch, including transfer counts and payload lengths. |
| Invalid/zero SWO baud | Reject before divisor arithmetic; use 32-bit SWO capacity/status fields. |
| Unstable AXI responses / coupled AW and W | Shared AXI-Lite register slave retains separate address/data and stalled responses. |
| SWO overflow/full push-pop accounting | Synchronous banked FIFO, packed pops and explicit drop/high-water/framing counters. |
| SWO draining blocked by SWD work | Independent software capture worker and 1 MiB ring. |
| Ineffective SWD disconnect | Release control overrides FPGA drive; SWCLK and SWDIO are tri-stated at top level. |
| Pod 2 GPIO-only backend | Shared FPGA SWD backend at `0x43C10000`; propagate memory-access errors and handle 1 KiB TAR boundaries. |
| Pod 2 parser/alignment issues | Validate bounds, read little-endian values without unaligned casts, retain atomic short responses. Legacy standalone server disabled. |
| Clock changed by running UART bridge | Clock setup/readback confined to approved verified loader with clients stopped. |
| Unauthenticated web writes | Bearer token, same-origin checks, size/time limits and fail-closed control responses. |
| Corrupted multipart firmware | MIME parsing preserves binary tails; unique uploads confined to the upload directory. |
| SSE whitespace loss / resource leaks | JSON-framed events, incremental UTF-8 decode, explicit UART channel and socket cleanup. |
| Unsafe T10 reuse | Input by default; BOOT0 writes blocked unless explicitly reconfigured after rewiring. |
| Reset high drive / failure cleanup | Assert using atomic low direction; release to input in a finally block. |
| Misleading dashboard states | Actual snapshots and freshness checks; failed GPIO reads report unknown rather than low. |
| Mismatched deployed artifacts | Matched release manifest, source and artifact verification, immutable release paths and readiness marker. |
| Shutdown or programming command hangs | Cooperative server termination releases sessions/pins; flasher subprocess groups have a five-minute deadline and bounded log reads. |
| Top-level SWCLK IOBUF invalid feedback | Route EMIO feedback from the IOBUF O pin rather than adding fabric loads to the external IO pad. |
| Tray reconnect/hub-reset hazards | Serialized attach/detach, exact-device matching, bounded commands, singleton; no routine hub restart/taskkill. |

## Still open / not established

- **Windows BSOD root cause:** bugcheck `0x0000000A` on 2026-11-18. No driver
  attribution has been established. Transport fixes do not prove it resolved.
- **Sustained-load qualification:** simultaneous SWD, SWO and dual CDC,
  disconnect/reconnect, slow-host and long-running tests still need approval.
- **UARTLite IRQ mode:** block-design IRQ wiring and an example device-tree
  include exist. Board Linux/device-tree integration remains unverified.
  MMIO polling fallback is not sustained-throughput qualified.
- **External SWD timing:** real target and cable timing data are required.
  No invented I/O constraints or blanket warning suppression are used.
- **Trace data loss:** counters make loss visible, not impossible. Unflowcontrolled
  UART/SWO inputs cannot backpressure a target; slow clients may be disconnected.
- **End-to-end delivery:** TCP acceptance and CDC OUT completion indicate queue
  acceptance, not proof that a target consumed or executed the data.
- **Target compatibility:** only documented historical STM32F407 observations
  are evidence. Broad MCU family support, multidrop and ST-Link compatibility
  are not qualified merely because an interface is advertised.
- **Future architecture:** command FIFOs, DMA, timestamped flight recorder,
  production programming and direct remote GDB integration are not implemented
  by this work.
