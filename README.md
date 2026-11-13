# Antminer T9+ Universal CMSIS-DAP v2 Debugger & Multi-Protocol Pod

Transform any Antminer T9+ Control Board (Xilinx Zynq XC7Z010-1CLG400C) into a turnkey, high-speed **CMSIS-DAP v2 Hardware Debug Probe**, dual TCP UART bridge, and target automation pod over Gigabit Ethernet.

---

## Key Highlights

- **Standard CMSIS-DAP v2 (USB/IP):** Exposes high-speed USB 2.0 (480 Mbps) bulk endpoints directly over IP. Zero driver installation needed on Windows (native WinUSB `0d28:0204`).
- **Complete STM32 Compatibility:** Works out of the box with **ALL STM32 families** (F0, F1, F2, F3, F4, F7, G0, G4, L0, L1, L4, L5, H7, U5, C0, WB, WL).
- **Broad ARM Cortex-M Ecosystem Support:** Native SWD support for **NXP** (LPC, i.MX RT), **TI** (MSP432, CC13xx/CC26xx), **Microchip/Atmel** (SAMD, SAME), **Raspberry Pi** (RP2040, RP2350), and **WCH** (CH32F series).
- **ESP32 Family UART Programming:** Full automated flashing for **ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6, and ESP8266** using `esptool.py` over TCP socket with automated bootloader sequencing.
- **Clean LED Signaling:**
  - **Bicolor LED D3 (PS MIO):** Solid OFF when idle; **blinks GREEN only** during active SWD transfers/debugging. Red LED remains completely OFF (no red/green flashing).
  - **PL User LEDs D4, D5, D6, D7 (D8):** Active-low pins are driven high (`1'b1`) in the FPGA bitstream, remaining **solid OFF by default**.
  - **D1 LED:** Solid GREEN indicates FPGA bitstream is loaded and operational.
- **Header J3 Pinout:** 3.3V LVCMOS SWD, primary UART, and target reset on standard 2.54mm dual-row header. No target `BOOT0` pin required.

---

## Quick-Start: Convert a Raw T9 Board to Automated Debugger

If you are starting from a raw Antminer T9+ board, follow these steps to deploy the turnkey debugger in under 5 minutes:

### Step 1: Install Base PYNQ Linux Image
Follow the base setup instructions from the **[Antminer T9+ PYNQ Devboard](https://github.com/P34SANT/t9plus-devboard.git)** repository:
1. Flash the PYNQ SD card image provided in `t9plus-devboard`.
2. Insert SD card into T9+, connect Ethernet, and power on.
3. Verify SSH connection:
   ```bash
   ssh xilinx@192.168.2.99
   # Default password: xilinx
   ```

### Step 2: Clone and Install `t9plus_debugger`
Run the automated turnkey installer directly on the board:
```bash
git clone https://github.com/P34SANT/t9plus_debugger.git
cd t9plus_debugger/t9_programmer
chmod +x deploy.sh
./deploy.sh
```

### Step 3: What the Installer Automates
The `deploy.sh` script installs and starts all systemd background daemons:
- **`t9-fpga.service`:** Automatically programs the Zynq PL bitstream on boot.
- **`t9-cmsis-dap.service`:** Starts the CMSIS-DAP v2 USB/IP server on port 3240.
- **`t9-uart-bridge.service`:** Bridges UART0 to TCP port 2000 and UART1 to TCP port 2001.
- **`t9-io-daemon.service`:** Exposes the REST API and target pin controller on port 8080.

---

## Hardware Pinout & Wiring (Header J3)

All SWD, UART, and target control signals are routed to the **J3 expansion header** (2x12 dual-row 2.54mm connector, 3.3V LVCMOS):

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
| **Pin 1, 2** | `GND` | **GND** | **Target GND** | Common Ground (Required) |
| **Pin 3** | `P16` | **SWCLK** | **Target SWCLK** (e.g. PA14) | SWD Clock (1 MHz default) |
| **Pin 4** | `P15` | **SWDIO** | **Target SWDIO** (e.g. PA13) | Bidirectional SWD Data (Internal pullup) |
| **Pin 5** | `T14` | **nRST** | **Target NRST / ESP32 EN** | Target Hardware Reset (Open-Drain) |
| **Pin 6** | `T10` | *Unused* | *Leave disconnected* | BOOT0 not used |
| **Pin 7** | `C20` | **UART TX** | **Target RX / ESP32 RXD0** | Primary UART TX (TCP port 2000) |
| **Pin 8** | `B20` | **UART RX** | **Target TX / ESP32 TXD0** | Primary UART RX (TCP port 2000) |
| **Pin 15** | `T15` | **TARGET_IO0**| **ESP32 GPIO0** *(Optional)* | Auxiliary Target Control / ESP32 Boot Mode |
| **Pin 16** | `VCC` | **+3.3V** | **Target 3.3V** *(Optional)* | Onboard 3.3V supply rail |
| **Pin 23, 24**| `GND` | **GND** | **Target GND** | Ground reference |

---

## Connecting from Windows / Linux Hosts

### 1-Click Windows USB/IP Attach
Run the provided batch script or PowerShell command:
```powershell
# Attach CMSIS-DAP over USB/IP
& "C:\Program Files\USBip\usbip.exe" attach -r 192.168.2.99 -b 1-1

# Verify attachment
& "C:\Program Files\USBip\usbip.exe" port
```
Windows automatically binds the probe under **Universal Serial Bus devices** as **ARM CMSIS-DAP** (`0d28:0204`).

### Using OpenOCD
Connect to any STM32 or ARM target:
```bash
openocd -f interface/cmsis-dap.cfg \
        -c "cmsis-dap backend usb_bulk; transport select swd" \
        -f target/stm32f4x.cfg \
        -c "adapter speed 1000; init; reset halt"
```

### Using IDEs (STM32CubeIDE / Keil µVision / IAR / CLion / VS Code)
1. In your project Debug Configuration, select **CMSIS-DAP**.
2. Set protocol to **SWD** and port to **USB**.
3. Select probe `ARM CMSIS-DAP (0204000100000000)`.
4. Click **Debug** / **Download**.
5. Observe **LED D3** blinking green during active flash and debugging operations!

---

## Target Compatibility Guide

### 1. STM32 Families (Full Support Out of the Box)
- **Supported:** STM32F0, F1, F2, F3, F4, F7, G0, G4, L0, L1, L4, L5, H7, U5, C0, WB, WL.
- **Why it works reliably:**
  - **Dormant-to-SWD Switching:** Fix in `dap_swj_sequence()` handles full 256-bit unlock sequences for modern low-power families (G0, G4, L5, U5, H7).
  - **Clock Domain Crossing:** Enforces 8 ADIv5 idle cycles (`SWDIO=0`) following AP transfers, preventing internal bus synchronization stalls (`ack=WAIT` / `ack=5`).
  - **Connect Under Reset:** Hardware `nRST` is driven open-drain (`zynq_pin_dir_in` when high, `zynq_set_pin(0)` when low), allowing clean connection to MCUs in low-power sleep modes or with disabled SWD pins.

### 2. Other ARM Cortex-M MCUs
- **NXP:** LPC55xx, LPC11xx, LPC8xx, i.MX RT10xx, Kinetis.
- **Texas Instruments:** MSP432, SimpleLink CC13xx, CC26xx, CC32xx.
- **Microchip/Atmel:** SAMD21, SAME54, SAMC21.
- **Raspberry Pi:** RP2040 (Cortex-M0+), RP2350 (Cortex-M33).
- **WCH:** CH32F103, CH32F203 (ARM Cortex-M cores).

### 3. ESP32 Family UART Flashing
All ESP32 chips are programmed via ROM UART bootloader using `esptool.py`. The Antminer T9+ provides automated bootloader entry and flashing over the network:

#### Wiring ESP32 to J3 Header:
- T9+ J3 Pin 7 (TX) -> ESP32 RXD0
- T9+ J3 Pin 8 (RX) -> ESP32 TXD0
- T9+ J3 Pin 5 (nRST) -> ESP32 EN (CHIP_PU)
- T9+ J3 Pin 15 (IO0) -> ESP32 GPIO0
- T9+ J3 Pin 1/2 (GND) -> ESP32 GND

#### Automated Flashing with `t9_esp32_flash.py`:
```bash
# Read chip flash info
python scripts/t9_esp32_flash.py --ip 192.168.2.99 flash_id

# Flash firmware image
python scripts/t9_esp32_flash.py --ip 192.168.2.99 write_flash 0x10000 app.bin

# Erase flash
python scripts/t9_esp32_flash.py --ip 192.168.2.99 erase_flash
```
The script calls `/api/esp32/bootloader` on the board to pull `GPIO0` low and pulse `EN`, connects `esptool.py` to `socket://192.168.2.99:2000`, and then pulses `EN` via `/api/esp32/reset` to start the application automatically!

---

## Status LED Behavior Reference

| LED | Source | Color | Idle State | Debugging / Flashing State | Description |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **D1** | FPGA Pin | Green | **Solid ON** | **Solid ON** | FPGA Done (Bitstream loaded) |
| **D3** | PS MIO 37/38 | Red/Green | **Solid OFF** | **Blinking GREEN** | CMSIS-DAP SWD Activity Indicator. Red is held OFF. |
| **D4** | PL Pin F16 | Yellow | **Solid OFF** | **Solid OFF** | Kept OFF (`1'b1` active-low) |
| **D5** | PL Pin M19 | Red | **Solid OFF** | **Solid OFF** | Kept OFF (`1'b1` active-low) |
| **D6** | PL Pin M17 | Green | **Solid OFF** | **Solid OFF** | Kept OFF (`1'b1` active-low) |
| **D7** | PL Pin L19 | Red | **Solid OFF** | **Solid OFF** | Kept OFF (`1'b1` active-low) |

---

## REST API & Web Dashboard

The onboard I/O daemon provides a REST API on port `8080`:

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/api/pins` | Get status of all target I/O pins |
| `POST` | `/api/pins/<name>` | Set target pin value (`{"value": 0 or 1}`) |
| `POST` | `/api/pins/<name>/pulse` | Pulse target pin (`{"duration_ms": 50}`) |
| `POST` | `/api/esp32/bootloader` | Enter ESP32 ROM bootloader mode (`GPIO0=0`, pulse `EN`) |
| `POST` | `/api/esp32/reset` | Reboot ESP32 into normal execution mode (pulse `EN`) |
| `POST` | `/api/programmer/bootloader` | Pulse hardware reset |
| `POST` | `/api/led/status?mode=idle` | Set LED D3 mode (`idle`, `off`, `programming`, `success`) |

---

## Repository Structure

```text
t9plus_debugger/
├── constraints/
│   ├── t9_programmer.xdc      # FPGA pin constraints for J3 header & PL LEDs
│   └── t9_v12_pins.xdc        # Master pin map reference for Antminer T9+ V1.2
├── scripts/
│   ├── t9_bit2pcap.py         # Vivado .bit to Zynq PCAP DMA .bin converter
│   ├── t9_load_bit.py         # Runtime Linux FPGA manager loader
│   └── t9_esp32_flash.py      # Network ESP32 automated programmer helper
├── t9_programmer/
│   ├── bin/                   # Helper CLI utilities (t9-reset, t9-io)
│   ├── io_daemon/             # Web/REST diagnostic daemon (port 8080)
│   ├── openocd/               # Standalone OpenOCD scripts
│   ├── systemd/               # Linux systemd service units
│   ├── uart_bridge/           # TCP to UART socket daemon (ports 2000/2001)
│   ├── usbip_dap/             # CMSIS-DAP v2 USB/IP engine source code
│   └── deploy.sh              # 1-step turnkey board installer
├── vivado/
│   ├── build_programmer_bitstream.tcl  # Automated Vivado build script
│   ├── create_programmer_project.tcl   # Block design generator
│   └── top_programmer.v       # Top-level Verilog wrapper (IOBUFs, LED assignments)
├── t9_programmer.bit          # Synthesized FPGA bitstream
└── t9_programmer_pcap.bin     # Ready-to-load PCAP binary
```

---

## License
MIT License.
