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

## Hardware Pinout & Target MCU Wiring (Header J3)

All programmer, debugger, and serial signals are routed to the **J3 expansion header** (2x12 dual-row 2.54mm connector, Banks 34 & 35, 3.3V LVCMOS).

### J3 Header Pinout Map
```text
 1    3    5    7    9    11   13   15   17   19   21   23
 2    4    6    8    10   12   14   16   18   20   22   24
┌────────────────────────────────────────────────────────┐
│GND  P16  T14  C20  GND  P14  GND  T15  T17  GND  M14  GND│
│GND  P15  T10  B20  GND  R14  GND  VCC  R18  GND  M15  GND│
└────────────────────────────────────────────────────────┘
```

| J3 Pin # | FPGA Pin | Signal Name | Target MCU Connection | Description |
| :---: | :---: | :--- | :--- | :--- |
| **Pin 1, 2** | `GND` | **GND** | **Target GND** | Common Ground reference |
| **Pin 3** | `P16` | **SWCLK** | **Target SWCLK** (e.g. PA14) | SWD Clock (3.3V LVCMOS) |
| **Pin 4** | `P15` | **SWDIO** | **Target SWDIO** (e.g. PA13) | Bidirectional SWD data (with pull-up) |
| **Pin 5** | `T14` | **nRST** | **Target NRST** *(Optional)* | Target reset (open-drain with pull-up) |
| **Pin 6** | `T10` | *Unused* | *Leave disconnected* | BOOT0 disabled |
| **Pin 7** | `C20` | **UART0 TX** | **Target RX** (e.g. PA10) | Probe Primary UART TX (TCP port 2000) |
| **Pin 8** | `B20` | **UART0 RX** | **Target TX** (e.g. PA9) | Target Primary UART TX (TCP port 2000) |
| **Pin 9, 10**| `GND` | **GND** | **Target GND** | Ground reference |
| **Pin 11** | `P14` | **UART1 TX** | **Secondary Target RX** | Probe Secondary UART TX (TCP port 2001) |
| **Pin 12** | `R14` | **UART1 RX** | **Secondary Target TX** | Target Secondary UART TX (TCP port 2001) |
| **Pin 16** | `VCC` | **+3.3V** | **Target 3.3V VDD** *(Optional)*| Onboard 3.3V power supply rail |

---

## Quick Start Guide

### 1. Connect Target to Probe (Header J3)
1. Connect **GND** (J3 Pin 1 or 2) to **Target GND**.
2. Connect **SWCLK** (J3 Pin 3) to **Target SWCLK**.
3. Connect **SWDIO** (J3 Pin 4) to **Target SWDIO**.
4. *(Optional)* Connect **nRST** (J3 Pin 5) to **Target NRST**.
5. *(Optional)* Connect **Target 3.3V VDD** to **J3 Pin 16** (or power target via USB).
6. *(Optional)* Connect **UART0 TX** (J3 Pin 7) to Target RX and **UART0 RX** (J3 Pin 8) to Target TX.

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
