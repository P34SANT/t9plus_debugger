# ABI 2 implementation and qualification

## Safety boundary

This pass is offline: no USB/IP operations, virtual hub resets, board service
restarts, target flashes or FPGA loads. A successful GitHub push is not a
deployment. Live tests need separate approval, particularly after the earlier
Windows crash.

## ABI and release

- SWD IDs: `0x53574402`, bases `0x43C00000` and `0x43C10000`.
- SWO ID: `0x53574F02`, base `0x43C20000`.
- Fabric clock configuration: 100 MHz.
- SWO hardware FIFO: 32 KiB; software capture ring: 1 MiB.
- CDC software rings: 64 KiB RX and TX per channel.
- DAP packet/reply count: eight; packet size: 512 bytes.
- Readiness file: `/run/plink/fpga-ready`, exactly `plink-abi-2\n`.

Do not run ABI 2 software over ABI 1 hardware. Only a verified release loader
writes readiness after loading matched PCAP and checking clock configuration.
Root `.bit`/`.hwh` files are build convenience copies, not release manifests.
The old root PCAP is stale and must not be deployed.

Source hashes normalize CRLF/LF text checkouts and include C headers/includes, Makefiles, Python, RTL, constraints,
systemd units, canonical hardware mapping, frontend assets and CLI scripts.
Artifact validation also checks PCAP corresponds to the `.bit` payload.
These hashes detect mismatch, not maliciously re-signed manifests.

## Test coverage

### Python

```text
python -m unittest discover -s tests -p "test_*.py" -v
```

Historical SWO tool tests require `pyusb`, `pyserial` and `libusb-package` on the
test host; their device operations are mocked. Linux-specific TTY/flock tests
skip on Windows and can run separately in WSL:

```text
python3 -m unittest discover -s tests -p "test_uart_and_ownership.py" -v
```

Coverage includes upload binary integrity, auth, artifact/source tampering,
HTTP asset allowlisting/security headers, canonical pin map, malformed JSON,
unknown states, reset cleanup, SSE framing, partial TTY writes, coalesced
bounded delivery and exclusive leases. Importing the daemon does not initialize
hardware; tests construct fake managers and bind only to localhost.

### C

Using a Linux compiler (or Zig `cc -target x86_64-linux-musl -static`):

```sh
cc -std=gnu11 -O1 -Wall -Wextra -pthread tests/test_transport.c \
  t9_programmer/stlink/stlink_swd.c -o /tmp/plink-test-transport
cc -std=gnu11 -O1 -Wall -Wextra -pthread tests/test_dap.c \
  t9_programmer/usbip_dap/swo_capture.c -o /tmp/plink-test-dap
cc -std=gnu11 -O1 -Wall -Wextra -pthread tests/test_swo_ring.c \
  -o /tmp/plink-test-swo-ring
```

Run each test binary. They use socketpairs, fake registers and fake handlers,
not real USB/IP or MMIO. Real DAP handler tests define `PLINK_OFFLINE_TEST`.
Assertions remain enabled even when the compiler defines `NDEBUG`.

The production server is also cross-compiled for
`arm-linux-musleabihf -mcpu=cortex_a9` with `-Wall -Wextra -pthread`.

### Go and browser

Run `go test ./...` inside `plink-tray` on Windows.
Exact pod URL matching and duplicate detection are tested without invoking
USB/IP.

`tests/ui_smoke.cjs` uses Playwright from an external install (`NODE_PATH`) and
an installed Edge browser (`PLINK_BROWSER` overrides its path). It starts a
local read-only mock preview and checks desktop/mobile layout, connector
mapping, unknown metrics, navigation and authentication gating. Set
`PLINK_UI_OUTPUT` to an existing directory for screenshots.

### RTL

Compile `vivado/axil_register_slave.v`, `vivado/axi_swd_engine.v`,
`vivado/axi_swo_engine.v`, then `tests/rtl/tb_pipeline.v` (SystemVerilog testbench)
with XSim; elaborate `tb_pipeline` and run all.

The bench covers UART byte decode, FIFO overflow/wrap/unaligned packed reads,
simultaneous full pop/push, stalled AXI responses, SWD raw/read completion and
release, plus 48 combinations of turnaround length, direction, OK/WAIT/FAULT
ACK and error data phase. It is not an exhaustive SWD protocol verification environment.

```text
vivado -mode batch -source tests/rtl/check_swo_synthesis.tcl -tclargs <repo>
```

Standalone synthesis confirms eight RAMB36 primitives for the 32 KiB FIFO.
Unused AXI protection-bit warnings are expected; internal timing and board-level
external timing must be reviewed separately.

## Web and operations

Mutations require `Authorization: Bearer <token>`. The UI stores no token in
URLs, localStorage or sessionStorage. It sets the token only in page memory.
Control requests with a foreign Origin are rejected. Use HTTPS or an isolated
trusted LAN; there is no built-in TLS.

`/api/status` reports Linux fabric state; `/api/metrics` returns snapshot ages;
`/api/pinmap` serves the canonical signal map. The UI treats snapshots older
than five seconds as stale. SWD clock is divider-derived, not scope-measured.
Pod 2 does not yet publish a corresponding full metrics snapshot.

The two raw TCP UART bridges bind to loopback. UART console SSE carries
JSON-encoded text and a selected channel. Firmware programming holds pod and
UART leases; current fixed CDC line coding is 115200 8N1, not arbitrary baud.

UARTLite kernel TTY use requires a verified board device tree. Review
`hardware/uartlite-linux.dtsi` against the actual board before applying it.
It is deliberately not automatically deployed.

## Required live qualification (not performed)

1. Preserve crash diagnostics; identify driver versions and analyze the dump.
2. Verify the matched release, pin states and reset release on a disconnected
   target before approving a load/connect operation.
3. Validate SWD turnaround/data-phase/WAIT/FAULT behavior against actual targets.
4. Compare SWO counters and decoded sequence IDs during long sustained capture.
5. Exercise concurrent dual CDC with short reads, slow consumers and reconnects.
6. Validate UARTLite interrupt mapping and Linux TTY operation.
7. Record cable/target timing, actual rates, queue high-water values, errors and
   driver behavior. Do not promote a short smoke test into a stability claim.
