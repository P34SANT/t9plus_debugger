# Antminer T9+ Universal CMSIS-DAP v2 Hardware Debug Probe

This turnkey subsystem transforms the Xilinx Zynq-7000 SoC on the Antminer T9+ control board into a high-speed **CMSIS-DAP v2 Hardware Debug Probe** running directly over USB/IP.

It exposes standard USB bulk endpoints (WinUSB) natively over IP, allowing IDEs on Windows or Linux to connect directly as if a physical hardware probe (such as an ST-LINK V3 or J-Link) were plugged into a physical USB port on the workstation.

---

## Features

- **Standard CMSIS-DAP v2 Protocol**: Full compatibility with IAR Embedded Workbench, Keil MDK, VS Code Cortex-Debug, STM32CubeIDE, CLion, and OpenOCD.
- **Direct Zynq EMIO SWD Driver**: High-speed memory-mapped GPIO register driving with calibrated cycle-accurate Cortex-A9 timing.
- **Hardware Reset Control (`nRST`)**: Target hardware reset on J3 Pin 3 driven via Zynq EMIO GPIO 56.
- **Active-Low User LEDs**: Bitstream natively inverts LED outputs (`~(cnt > 0)`) so onboard LEDs D4, D5, D6, D7 remain turned OFF when idle and pulse only on UART traffic.
- **No BOOT0 Requirement**: Target BOOT0 pin is completely untouched.
- **Automatic Systemd Service**: Runs on boot as `t9-cmsis-dap.service` on TCP port 3240.

---

## Physical Pinout (J3 Target Header)

| Pin | Signal | Zynq EMIO Pin | Description |
|-----|--------|---------------|-------------|
| **J3 Pin 1** | `SWCLK` | GPIO 54 (Bank 2, Bit 0) | SWD Clock |
| **J3 Pin 2** | `SWDIO` | GPIO 55 (Bank 2, Bit 1) | Bidirectional SWD Data |
| **J3 Pin 3** | `nRST`  | GPIO 56 (Bank 2, Bit 2) | Target Hardware Reset |
| **J3 Pin 5** | `GND`   | Ground        | Common Reference Ground |

*(Pins J3.4, J3.6, etc., provide 3.3V power, auxiliary UARTs, and user I/Os).*

---

## Quick Start (Windows)

### 1. Attach the Probe over USB/IP

From PowerShell or Command Prompt on Windows:

```powershell
& "C:\Program Files\USBip\usbip.exe" attach -r <BOARD_IP> -b 1-1
```

For example:
```powershell
& "C:\Program Files\USBip\usbip.exe" attach -r 192.168.2.99 -b 1-1
```

The device immediately appears in Windows Device Manager under **Universal Serial Bus devices** as **CMSIS-DAP** (`VID: 0x0D28, PID: 0x0204`).

To detach when finished:
```powershell
& "C:\Program Files\USBip\usbip.exe" detach -p 1
```

---

## IDE Configuration

### 1. IAR Embedded Workbench
1. Open your project options (**Project -> Options**).
2. Under **Debugger -> Setup**, select **CMSIS-DAP** as the Driver.
3. Under **Debugger -> CMSIS-DAP**:
   - Interface: **SWD**
   - Reset: **System (SYSRESETREQ)** or **Normal**
   - SWD Frequency: **1 MHz - 2 MHz**
4. Click **Download and Debug** (`Ctrl + D`).

### 2. Keil MDK-ARM
1. Open **Options for Target** -> **Debug**.
2. Select **CMSIS-DAP Debugger** / **CMSIS-DAP ARMv8-M**.
3. Click **Settings**:
   - Port: **SW**
   - Max Clock: **1 MHz** or **2 MHz**
   - Reset: **SYSRESETREQ** or **HW RESET**
4. The SW Device ID (`0x2BA01477` for Cortex-M4) will appear in the SW Device list.

### 3. OpenOCD on Windows
```powershell
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f4x.cfg -c "reset_config none; adapter speed 1000"
```

---

## Linux Service Management on Board

The probe server is managed by systemd:

```bash
# Check probe status
systemctl status t9-cmsis-dap.service

# Restart probe server
sudo systemctl restart t9-cmsis-dap.service

# View live probe logs
journalctl -u t9-cmsis-dap.service -f
```
