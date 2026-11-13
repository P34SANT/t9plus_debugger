# t9plus_debugger

Universal CMSIS-DAP v2 Hardware Debug Probe & Dual-Channel UART Pod on the Antminer T9+ Control Board (Xilinx Zynq XC7Z010-1CLG400C).

---

## Overview

`t9plus_debugger` repurposes the low-cost Antminer T9+ mining control board into a high-speed, network-attached CMSIS-DAP v2 hardware debug probe and dual-channel serial communication pod.

- **Interface:** High-Speed USB 2.0 (480 Mbps) emulated over **USB/IP** (WinUSB compatible).
- **Core Engine:** Native ARM CMSIS-DAP v2 protocol server interfacing directly with Zynq EMIO GPIOs and PL IOBUFs.
- **Physical Connector:** Standard 2.54mm 4-pin **Fan Headers** (Bank 34/35 3.3V LVCMOS).
- **Toolchain Support:** Zero-driver, plug-and-play compatibility with **STM32CubeIDE**, **Keil µVision**, **IAR Embedded Workbench**, **OpenOCD**, and **VS Code Cortex-Debug**.
- **Integrated Dual UART:** Target console serial streams exposed over raw TCP sockets for headless debugging.

---

## Fan Headers (Reusable as 3.3V GPIO, Bank 34)

The Antminer T9+ provides 4 dedicated 4-pin fan headers wired to 3.3V LVCMOS I/O on Bank 34/35. Pin 4 (`J18`) is physically tied across all four fan headers on the PCB.

```text
FAN6: Pin 3 -> H20 (L17N), Pin 4 -> J18 (L14P)
FAN5: Pin 3 -> G17 (L16P), Pin 4 -> J18 (L14P)
FAN4: Pin 3 -> J20 (L17P), Pin 4 -> J18 (L14P)
FAN3: Pin 3 -> F20 (L15N), Pin 4 -> J18 (L14P)
```

---

## Hardware Pinout & Target MCU Wiring

> ⚠️ **CRITICAL WARNING — PIN 2 IS +12V**:  
> On every fan header, **Pin 2 carries +12V** (fan power rail). **NEVER connect Pin 2 to your STM32 or any 3.3V target!**

| Fan Header | Pin # | FPGA Pin | Signal Name | Target MCU Connection | Description |
| :--- | :---: | :---: | :--- | :--- | :--- |
| **FAN3** | **Pin 1** | `GND` | **GND** | **Target GND** | Common Ground reference |
| **FAN3** | **Pin 2** | `+12V` | *+12V Fan Rail* | **DO NOT CONNECT!** | High-voltage supply rail |
| **FAN3** | **Pin 3** | `F20` | **SWDIO** | **Target SWDIO** (e.g. PA13) | Bidirectional SWD data (3.3V with pull-up) |
| **FAN3** | **Pin 4** | `J18` | **SWCLK** | **Target SWCLK** (e.g. PA14) | SWD Clock output (3.3V LVCMOS) |
| | | | | | |
| **FAN4** | **Pin 3** | `J20` | **nRST** | **Target NRST** | Target reset (open-drain with pull-up) |
| | | | | | |
| **FAN5** | **Pin 3** | `G17` | **UART0 TX** | **Target RX** (e.g. PA10) | Probe TX &rarr; Target MCU RX |
| **FAN6** | **Pin 3** | `H20` | **UART0 RX** | **Target TX** (e.g. PA9) | Target MCU TX &rarr; Probe RX (with pull-up) |

### Single-Connector SWD:
Because **Pin 4 (`J18`)** is SWCLK and **Pin 3 (`F20`)** is SWDIO, core SWD programming requires only **FAN3** (Pins 1, 3, and 4).

---

## Quick Start Guide

### 1. Connect Target to Probe
1. Connect **GND** (FAN3 Pin 1) to Target GND.
2. Connect **SWDIO** (FAN3 Pin 3) to Target SWDIO.
3. Connect **SWCLK** (FAN3 Pin 4) to Target SWCLK.
4. *(Optional)* Connect **nRST** (FAN4 Pin 3) to Target NRST.
5. *(Optional)* Connect **UART0 TX** (FAN5 Pin 3) and **RX** (FAN6 Pin 3).
6. Power your STM32 target board (via USB or external 3.3V).

### 2. Attach CMSIS-DAP over USB/IP (Windows Host)
Ensure `usbip-win` is installed. Run PowerShell as Administrator:

```powershell
# Query available devices on the T9+ board (Default IP: 192.168.2.99)
& "C:\Program Files\USBip\usbip.exe" list -r 192.168.2.99

# Attach the CMSIS-DAP v2 probe
& "C:\Program Files\USBip\usbip.exe" attach -r 192.168.2.99 -b 1-1
```

Verify in Windows Device Manager:
- Under **Universal Serial Bus devices**, you will see **ARM CMSIS-DAP** operating via Microsoft's default WinUSB driver.

### 3. Connect from Your IDE / OpenOCD

#### OpenOCD:
```bash
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap backend usb_bulk; transport select swd; adapter speed 2000" -f target/stm32f4x.cfg
```

#### Keil MDK / STM32CubeIDE / IAR:
1. Open Project Debug Settings.
2. Select **CMSIS-DAP Debugger** as the debug interface.
3. The probe will appear as `ARM CMSIS-DAP (0204000100000000)`.
4. Choose **SWD** mode and target clock (up to 4 MHz).
5. Click **Download & Debug**.

---

## Architecture & Features

### CMSIS-DAP v2 Engine (`t9_programmer/usbip_dap/`)
- Native High-Speed USB 2.0 (480 Mbps) emulation over USB/IP protocol.
- 512-byte bulk transfer endpoints with multi-slot FIFO URB queueing, preventing host buffer underflows and driver hangs during heavy flash writes.
- Fixed microsecond timing primitives in `dap_swj_pins` for sub-millisecond target reset and sync sequences.
- Onboard PL user LEDs (D5-D8) driven solid low (`1'b0`) to eliminate stray blinking or floating pin activity.

### Dual UART Bridge (`t9_programmer/uart_bridge/`)
- Exposes Primary UART (`/dev/ttyPS0`) on TCP port `2000`.
- Exposes Secondary UART (`/dev/ttyUL0`) on TCP port `2001`.
- Accessible directly via PuTTY (Raw Mode) or `nc 192.168.2.99 2000`.

---

## Repository Structure

```text
t9plus_debugger/
├── constraints/
│   ├── t9_programmer.xdc      # FPGA pin constraints for Fan Headers & PL LEDs
│   └── t9_v12_pins.xdc        # Master pin map reference for Antminer T9+ V1.2
├── scripts/
│   ├── t9_bit2pcap.py         # Vivado .bit to Zynq PCAP DMA .bin converter
│   ├── t9_load_bit.py         # Runtime Linux FPGA manager loader
│   └── test_swd_diag.py       # Hardware SWD pin verification diagnostic script
├── t9_programmer/
│   ├── bin/                   # Helper CLI utilities (t9-reset, t9-io)
│   ├── io_daemon/             # Web/REST diagnostic daemon
│   ├── openocd/               # Standalone OpenOCD scripts
│   ├── systemd/               # Linux systemd service units
│   ├── uart_bridge/           # TCP to UART socket daemon
│   └── usbip_dap/             # CMSIS-DAP v2 USB/IP engine source code
├── vivado/
│   ├── build_programmer_bitstream.tcl  # Automated Vivado build script
│   ├── create_programmer_project.tcl   # Block design generator
│   └── top_programmer.v       # Top-level Verilog wrapper (IOBUFs, Fan Pin routing)
├── deploy_to_board.ps1        # Deployment script to target board
├── t9_programmer.bit          # Synthesized FPGA bitstream
└── t9_programmer_pcap.bin     # Ready-to-load PCAP binary
```

---

## License
MIT License.
