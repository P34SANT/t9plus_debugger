# Antminer T9+ Debugger & Programmer - Bug Audit & Resolution Report

## 1. Summary of All Identified and Fixed Bugs

### Bug 1: TCP Stream Desynchronization on Bulk OUT Packet Oversize
- **File:** `t9_programmer/usbip_dap/usbip_server.c`
- **Issue:** When a host submitted an EP1 OUT transfer with `transfer_buffer_length > DAP_MAX_PACKET` (e.g. 1024 bytes), the server only read 512 bytes, leaving remaining bytes in the TCP socket. On the next loop iteration, `cmd_code` was read from stale payload data, destroying framing.
- **Resolution:** Added socket draining for any excess payload bytes (`rlen > to_read`), keeping the TCP stream in exact alignment.

### Bug 2: USB/IP Bulk IN URB Buffer Overrun
- **File:** `t9_programmer/usbip_dap/usbip_server.c`
- **Issue:** Returning `actual_length = dap_resp_len` when the host's IN URB allocated a smaller buffer (`cmd.transfer_buffer_length`) triggered a kernel `STATUS_BUFFER_OVERFLOW` / `-EOVERFLOW` error.
- **Resolution:** Clamped all IN completions (both direct and queued) with:
  `uint32_t tx_len = (dap_resp_len > cmd.transfer_buffer_length) ? cmd.transfer_buffer_length : dap_resp_len;`

### Bug 3: `dap_transfer_block` Buffer Limit Silent Dropping & Zero-Filling
- **File:** `t9_programmer/usbip_dap/DAP.c`
- **Issue:** The read and write loops in `dap_transfer_block` lacked `!dap_buf_error` and `!dap_abort` guards. Read operations exceeding packet size dropped words while reporting a false `resp_count`. Truncated write requests continued looping and wrote zeros to target memory.
- **Resolution:** Added buffer boundary guards to both read and write loops in `dap_transfer_block`, breaking immediately and returning `DAP_TRANSFER_ERROR` on overflow/underflow.

### Bug 4: Unbounded Infinite `while` Polling Loops
- **File:** `t9_programmer/usbip_dap/DAP.c`
- **Issue:** Hardware SWD polling loops in `dap_swj_sequence` and `dap_swd_sequence` polled `(status & 1)` without a timeout, causing infinite 100% CPU lockup if the FPGA clock stopped or bus glitched.
- **Resolution:** Replaced all unbounded `while` loops with bounded timeout iterations (`100,000` cycles).

### Bug 5: Omitted Timeout Check in `dap_swd_operation`
- **File:** `t9_programmer/usbip_dap/DAP.c`
- **Issue:** If the hardware FSM failed to complete within 100,000 cycles, the code did not check `(st & 1)` and interpreted in-flight bits as valid ACK.
- **Resolution:** Added `if (st & 1) return DAP_TRANSFER_ERROR;`.

### Bug 6: FPGA RTL Arithmetic Underflow on `cfg_idle == 0`
- **File:** `vivado/axi_swd_engine.v`
- **Issue:** `cfg_idle - 1'b1` underflowed from 0 to 31 in 5-bit arithmetic, executing 32 idle cycles instead of 0.
- **Resolution:** Added guard: `if (cfg_idle == 5'd0 || idle_cnt >= (cfg_idle - 1'b1))`.

### Bug 7: FPGA RTL Arithmetic Underflow on `reg_raw_len == 0`
- **File:** `vivado/axi_swd_engine.v`
- **Issue:** `reg_raw_len - 1'b1` underflowed from 0 to 63 in 6-bit arithmetic, executing 64 raw cycles instead of 0.
- **Resolution:** Added guard: `if (reg_raw_len == 6'd0 || bit_idx >= (reg_raw_len - 1'b1))`.

### Bug 8: AXI UARTLite Bridge Unconditional Write on Full FIFO
- **File:** `t9_programmer/uart_bridge/uart_bridge.py`
- **Issue:** If the Tx FIFO remained full after 1000 poll cycles, `write_tx()` unconditionally wrote to register `0x04`, corrupting or dropping characters.
- **Resolution:** Added full-flag verification; characters are only written when FIFO space is confirmed.

---

## 2. Dynamic LED Indicator System (Bicolor D3)

- **Target Header D3 LEDs:**
  - Red: PS MIO 37 (`Bank 1, bit 5`)
  - Green: PS MIO 38 (`Bank 1, bit 6`)
- **Visual Modes:**
  1. **Flash Programming Mode:**
     - Triggered automatically by bulk write transfers (`ID_DAP_TRANSFER_BLOCK` write with count > 4).
     - **Behavior:** Blinks **GREEN ONLY** (fast 60ms toggle). Red LED stays completely OFF.
  2. **Interactive Debugging Mode:**
     - Triggered by register reads/writes, stepping, breakpoints, and core polling.
     - **Behavior:** Blinks **BOTH ALTERNATING (ping-pong)**: Green ON / Red OFF <-> Green OFF / Red ON (80ms cycle). They are **never on at the same time**.
  3. **Idle Mode:**
     - Triggered when no bus activity is received for > 100ms or when disconnected.
     - **Behavior:** Both Green and Red turn OFF completely.
