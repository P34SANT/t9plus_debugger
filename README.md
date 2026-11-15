# Plink // Multi-Protocol Hardware Debug Probe & Programmer Pod
**Turnkey FPGA-Accelerated Debugger, Dual UART & Multi-Target Programmer on Antminer T9+**

![Plink Logo](plink_logo.svg)

Transform any Antminer T9+ Control Board (Xilinx Zynq XC7Z010-1CLG400C) into **Plink** — a high-speed **CMSIS-DAP v2 Hardware Debugger**, **Native USB Virtual COM Port (CDC-ACM)**, and web-driven multi-target programming pod over Gigabit Ethernet.

---

## Key Features

- **FPGA Hardware SWD Acceleration (`axi_swd_engine.v`):** Custom RTL core synthesized in Zynq PL at `0x43C00000`. Supports dynamic clock scaling from **100 kHz up to 25.0 MHz** on the fly with zero CPU jitter and timing closure.
- **Native Windows USB Composite Device (USB/IP):**
  - **Interface 0:** WinUSB CMSIS-DAP v2 (Microsoft OS 2.0 & 1.0 descriptors with GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`). Automatic driver binding on Windows 7, 8.1, 10, and 11.
  - **Interface 1 & 2:** Standard USB CDC-ACM Virtual COM Port (`usbser.sys`). Enumerate a **native `COMx` port** in Windows Device Manager automatically.
- **Dynamic Bicolor Status Signaling (LED D3):**
  - **Flash Programming:** Blinks **GREEN ONLY** (fast 60ms toggle). Red LED stays strictly OFF.
  - **Interactive Debugging:** Blinks **alternating Green & Red** (Green ON / Red OFF $\leftrightarrow$ Green OFF / Red ON, 80ms cycle). Never illuminated at the same time.
  - **Idle / Disconnected:** Both LEDs OFF (target pins safely tristated).
- **Zero-Polling Windows Tray Utility (`plink-tray.exe`):**
  - Lightweight system tray app built in **Go 1.20** (natively supporting Windows 7 through 11).
  - Passive TCP socket event wait: **0% CPU consumption and zero network polling packets**. Automatically attaches USB/IP when the pod powers on.
- **Tabbed Web Management Interface (Port 8080):**
  - **Tab 1 (Dashboard & States):** System architecture state, hardware controls (nRST pulse, BOOT0, ESP32 handshake), and embedded live UART0 serial terminal.
  - **Tab 2 (UART Programming):** Multi-engine flasher supporting STM32 (`stm32flash`) and ESP32 (`esptool.py`) with drag-and-drop file upload (`.hex`, `.bin`, `.elf`), baud rate configuration (9600 to 921600), and real-time Server-Sent Events (SSE) execution log streaming.
  - **Tab 3 (Pin Telemetry & Hardware):** Live voltage/logic level monitoring of all J3/J4 header pins.

---

## Device Support & Testing Status

| Device / Architecture | Protocol / Interface | Status | Test Details & Verification Notes |
| :--- | :--- | :---: | :--- |
| **STM32F407VG (Cortex-M4)** | SWD (CMSIS-DAP v2) | **TESTED & VERIFIED** | **100% verified** at 25.0 MHz. 1024 KiB flash programmed & verified via OpenOCD. Single-step debugged in IAR EWARM & pyOCD. |
| **STM32F4 Series (F401, F405, F411, F429)** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Same CoreSight ADIv5 debug architecture as F407. Uses standard STM32 flash loader algorithms. |
| **STM32F1 Series (F103 "BluePill", F100, F105)** | SWD & UART Bootloader | **SUPPORTED** | SWD via OpenOCD; UART bootloader via onboard `stm32flash` on J3 pins 7/8. |
| **STM32F0 / F2 / F3 / F7 Series** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Standard ARM SWD DPIDR / MEM-AP protocol. Verified protocol compliance. |
| **STM32G0 / G4 / L0 / L1 / L4 / L5 Series** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Low-power Cortex-M0+/M4/M33. Fully supported via IDE flash algorithms. |
| **STM32H7 Series (Dual/Single Core)** | SWD (CMSIS-DAP v2) | **SUPPORTED** | CoreSight DPv2 / SWD multi-drop architecture compatible. |
| **ESP32 (ESP-WROOM-32, ESP32-D0WD)** | UART ROM Bootloader | **SUPPORTED & TESTED** | Flashed via onboard `esptool.py v4.7.0` with automated IO0 (J3-15) and EN (J3-5) hardware reset. |
| **ESP32-S2 / ESP32-S3 / ESP32-C3 / ESP32-C6** | UART ROM Bootloader | **SUPPORTED** | Fully supported by `esptool.py` over primary UART0 at up to 921,600 baud. |
| **ESP8266 (ESP-01, ESP-12E/F)** | UART ROM Bootloader | **SUPPORTED** | Handshake via IO0 and RST pins using onboard `esptool.py`. |
| **Raspberry Pi RP2040 / RP2350** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Multi-drop SWD supported by CMSIS-DAP v2 protocol. |
| **Nordic nRF52832 / nRF52840** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Cortex-M4 SWD standard target. Compatible with Nordic nRF Command Line Tools. |
| **NXP LPC / i.MX RT (Cortex-M7)** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Standard CoreSight debug port. Supported via MCUXpresso, Keil, and IAR. |
| **Microchip SAMD21 / SAME54** | SWD (CMSIS-DAP v2) | **SUPPORTED** | Standard ARM CoreSight target. |
| **WCH CH32V (RISC-V)** | Proprietary 1-Wire SDI | *NOT TESTED* | Non-standard 1-wire debug protocol. Requires dedicated SDI PHY core (planned Phase 2B). |

---

## Hardware Pinout (Header J3)

All SWD, UART, and target control lines are routed to the **J3 expansion header** (2x12 dual-row 2.54mm header, 3.3V LVCMOS on Banks 34 & 35):

```text
 1    3    5    7    9    11   13   15   17   19   21   23
 2    4    6    8    10   12   14   16   18   20   22   24
┌────────────────────────────────────────────────────────┐
│GND  P16  T14  C20  GND  P14  GND  T15  T17  GND  M14  GND│
│GND  P15  T10  B20  GND  R14  GND  VCC  R18  GND  M15  GND│
└────────────────────────────────────────────────────────┘
```

| J3 Pin # | FPGA Pin | Signal Name | Target MCU Connection | Function & Electrical Characteristics |
| :---: | :---: | :--- | :--- | :--- |
| **Pin 1, 2** | `GND` | **GND** | **Target GND** | System Ground (Required) |
| **Pin 3** | `P16` | **SWCLK** | **Target SWCLK** | SWD Clock Output (FPGA Core, 100 kHz - 25 MHz, 3.3V) |
| **Pin 4** | `P15` | **SWDIO** | **Target SWDIO** | Bidirectional Data Line (FPGA Core, 3.3V with PULLUP) |
| **Pin 5** | `T14` | **nRST / EN** | **Target nRST / ESP32 EN** | Open-Drain Reset Line (Active Low, 50ms hardware pulse) |
| **Pin 6** | `T10` | **BOOT0** | **STM32 BOOT0** | Hardware Bootloader Select (High: ROM, Low: User Flash) |
| **Pin 7** | `C20` | **UART0_TX**| **Target RXD** | Primary UART TX (Zynq PS `/dev/ttyPS1`, 9600-921600 baud) |
| **Pin 8** | `B20` | **UART0_RX**| **Target TXD** | Primary UART RX (Zynq PS `/dev/ttyPS1`, 9600-921600 baud) |
| **Pin 11**| `P14` | **UART1_TX**| **Auxiliary RXD** | Secondary UART TX (AXI UARTLite, 115200 baud) |
| **Pin 12**| `R14` | **UART1_RX**| **Auxiliary TXD** | Secondary UART RX (AXI UARTLite, 115200 baud) |
| **Pin 15**| `T15` | **IO0** | **ESP32 GPIO0** | Auto-Download Bootloader Handshake for ESP32 |
| **Pin 16**| `VCC` | **VCC** | *(Optional)* | 3.3V Power Output |

---

## Baud Rate Support & Characteristics

| Channel | Physical Interface | Baud Rate Range | Implementation & Constraints |
| :--- | :--- | :--- | :--- |
| **Primary UART0** | J3 Pins 7 & 8 | **9,600 to 921,600+ baud** (Dynamic) | Driven by Zynq PS hardware UART controller (`/dev/ttyPS1`). Full dynamic clock divisor supporting all standard baud rates. Bridged to native Windows `COMx` and TCP `:2000`. |
| **Secondary UART1**| J3 Pins 11 & 12| **115,200 baud** (Fixed) | Driven by AXI UARTLite PL core. Fixed clock divider synthesized in bitstream. Bridged to TCP `:2001`. |

---

## Windows Client Setup

### Automatic Connection (Recommended)
1. Run **`plink-tray.exe`** (in project root).
2. It appears in the notification tray and automatically links USB/IP when the pod is detected.
3. Windows Device Manager will show:
   - `Plink CMSIS-DAP v2` under **Universal Serial Bus devices** (WinUSB).
   - `USB Serial Device (COMx)` under **Ports (COM & LPT)** (native CDC-ACM).

### Manual Batch Script
Run `attach_t9.bat` from Windows cmd or PowerShell to link USB/IP on demand.

---

## Web Dashboard & Flasher
Access the web dashboard at:
```text
http://192.168.2.99:8080
```
- **01 // Dashboard & States:** Live system diagnostics, hardware reset controls, and embedded serial terminal.
- **02 // UART Programming:** High-speed firmware flashing for STM32 and ESP32 with real-time log streaming.
- **03 // Pin Telemetry & Hardware:** Live J3 header logic levels.
