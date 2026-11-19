# Plink

**An FPGA-based CMSIS-DAP debug probe and dual serial adapter for the Antminer T9+.**

![Plink](plink_logo.svg)

Plink combines CMSIS-DAP SWD, asynchronous SWO capture and two 115200-baud
serial channels over Ethernet. Windows clients use standard WinUSB and CDC-ACM
interfaces through USB/IP. The web interface provides pinouts, serial terminals,
firmware programming and device status.

## Quick Start

**Already configured board?** Install the Windows driver once, connect Ethernet,
then run the tray EXE and select **CMSIS-DAP** in your IDE. Skip the deployment
step unless you are installing or updating the board software.

### 1. Install the Windows USB/IP driver

1. Download the **signed installer** for your Windows architecture from
   [USB/IP Client for Windows releases](https://github.com/vadimgrn/usbip-win2/releases).
   Use a supported Windows 10/11 installation; follow the release's requirements.
2. Create a Windows restore point and close active USB transfers before installing.
   The upstream installer restarts USB hubs during installation.
3. Run the installer as administrator. Keep the default installation folder:
   `C:\Program Files\USBip`.
4. Restart Windows if the installer requests it.

Use the signed release; **test-signing mode is not part of this setup**.
This is the Windows **USB/IP client**, not `usbipd-win` for sharing local USB
devices into WSL.

After attachment, Windows normally supplies **WinUSB** for the CMSIS-DAP
interface and **usbser.sys** for the two COM ports. Do not replace the entire
composite device or its serial interfaces with Zadig.

### 2. Connect the board

- Power the T9+ control board using its appropriate supply and connect Ethernet.
- The configured board address is **`192.168.2.99`**.
- For a direct Ethernet connection, give the PC's board-facing adapter a free
  address such as **`192.168.2.10`**, subnet mask **`255.255.255.0`**.
  A gateway is not needed on that adapter for the direct connection.

Check connectivity from PowerShell:

```powershell
ping 192.168.2.99
```

On an already configured board, open **http://192.168.2.99:8080**.
The web interface needs no API token.

### 3. Deploy to the board — first installation or software update

**Skip this step for the already configured board.** These commands assume a
T9+ already running compatible Linux with SSH access and `sudo`; they do not
create an SD-card image or install the board's operating system.

For deployment, the PC needs Git, Python 3, OpenSSH (`ssh` and `scp`) and Vivado
with Zynq-7000 support. The board needs Python 3, Bash, GNU make, GCC and
`flock`. Close the IDE/serial sessions, detach Plink and exit the tray before
replacing a running FPGA/probe release.

From PowerShell:

```powershell
git clone https://github.com/P34SANT/t9plus_debugger.git
cd t9plus_debugger

# Confirm SSH access. Use your board account if it differs.
ssh xilinx@192.168.2.99
# Exit the SSH shell before continuing with the Windows commands.
```

Open a shell where `vivado` is on PATH, then build a **matched** release:

```powershell
vivado -mode batch -source vivado/build_programmer_bitstream.tcl
```

The build writes `releases/<timestamp>`. Set `$release` to that actual directory:

```powershell
$release = ".\releases\<timestamp>"

# Verify only; does not touch the board.
.\deploy_to_board.ps1 -ReleasePath $release

# Deploy and load the verified FPGA/software release.
.\deploy_to_board.ps1 -ReleasePath $release -AllowLiveHardware
```

Enter the board's sudo password when prompted. For a different board/account,
add `-BoardIP <address> -User <account>`. The tray and batch file use
`192.168.2.99` by default and must be updated separately for another address.

**Use the release produced from the same source checkout.** Old release
manifests will not verify against changed sources. Do not deploy a loose root
`.bit` or the legacy root PCAP file.

UART1's native Linux driver is a **separate installation**, not included in the
base deployment. On a fresh board, follow the
[UARTLite build/install guide](linux/uartlite/README.md) with a module matching
the board's exact running kernel. The configured board already has it installed.
Do not force-load a module built for another kernel.

Check the base services:

```powershell
ssh xilinx@192.168.2.99 "systemctl is-active t9-fpga t9-cmsis-dap t9-uart-bridge t9-io-daemon"
```

Each should print `active`. With the native UART integration installed,
`systemctl is-active plink-uartlite` should also report `active`.
If updating an existing installation with the separate web override, review
[the web deployment notes](docs/web-ui-20261119.md) before replacing its base
release; that override verifies a specific base manifest.

### 4. Run the Windows tray EXE

**Recommended:** run [`plink-tray/plink-tray.exe`](plink-tray/plink-tray.exe).
Keep `plink.ico` alongside it when copying the app elsewhere.

```powershell
Start-Process .\plink-tray\plink-tray.exe
```

The application appears in the Windows notification area, not as a normal
window. It auto-attaches the Plink composite device when the board is available.
Right-click the icon for **Attach USB/IP Device Now** or **Detach USB/IP Device**.
Double-click it to open the web interface. Use the web interface for pin/reset
controls.

**Alternative:** with the tray closed, run `attach_t9.bat`, or attach manually:

```powershell
& "C:\Program Files\USBip\usbip.exe" list -r 192.168.2.99
& "C:\Program Files\USBip\usbip.exe" attach --once -r 192.168.2.99 -b 1-1
& "C:\Program Files\USBip\usbip.exe" port
```

Attach **`1-1`** for CMSIS-DAP and both serial ports. Do not run another attach
if `port` already lists `usbip://192.168.2.99:3240/1-1`.

After attachment, Device Manager should show:

| Interface | Use |
|---|---|
| Plink / CMSIS-DAP, WinUSB (`VID 0D28`, `PID 0204`) | Select this probe in the IDE |
| USB Serial Device, UART0 | J3 pins 7/8; COM9 on the tested PC |
| USB Serial Device, UART1 | J3 pins 11/12; COM10 on the tested PC |

**COM numbers vary between PCs.** Use **115200 baud, 8N1, no flow control**.
The debugger is the CMSIS-DAP interface, **not a COM port**.

### 5. Wire the target

With power off, connect the primary target to **J3**:

| Plink | Target | STM32F407 example |
|---|---|---|
| J3.1 or J3.2 | GND | GND |
| J3.3 | SWCLK | PA14 |
| J3.4 | SWDIO | PA13 |
| J3.5 | nRST, recommended | NRST |
| J3.6 | SWO, optional | PB3 |

Power the target appropriately and use **3.3 V logic only**. Do not assume the
probe will power your target. Leave BOOT0 in the target's normal boot setting;
SWD debugging does not require the UART bootloader. **J3.6 is SWO, not BOOT0.**
The web **Pinout** page shows both connectors in their physical 2×12 layout.

### 6. Select CMSIS-DAP in your IDE

Use **SWD**, start at **1 MHz**, and select the correct target MCU/flash algorithm.
Only one debugger should use the probe at a time. Menu names vary by IDE version;
use a version supporting **CMSIS-DAP v2 / WinUSB**.

#### IAR Embedded Workbench for Arm

1. Open **Project → Options → General Options → Target** and select your MCU.
2. Under **Debugger → Setup**, set **Driver** to **CMSIS DAP**.
3. Under **Debugger → CMSIS DAP → JTAG/SWD**, select **SWD** and **1 MHz**.
4. In the CMSIS-DAP probe selection, choose **Plink** if multiple probes are listed.
5. Use **System / SYSRESETREQ** initially. Hardware-reset options require J3.5
   to be connected to the target's NRST.
6. Under **Debugger → Download**, enable **Use flash loader(s)** for programming.
7. Build, then choose **Project → Download and Debug**.

#### Keil MDK / µVision

1. Open **Project → Options for Target → Device** and select your MCU.
2. On the **Debug** tab, select **Use: CMSIS-DAP Debugger**, then **Settings**.
3. Select the **Plink** debug unit, set **Port: SW** and **Max Clock: 1 MHz**.
4. Check that the target appears in the **SW Device** list.
5. In **Flash Download**, add the flash programming algorithm for your MCU.
   Under **Utilities**, use the target driver for flash programming.
6. Build, then use **Flash → Download** or **Debug → Start/Stop Debug Session**.

#### OpenOCD, VS Code and pyOCD

With OpenOCD installed, this STM32F4 example checks target discovery without
programming flash:

```powershell
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/stm32f4x.cfg -c "adapter speed 1000; init; targets; shutdown"
```

For **VS Code Cortex-Debug**, use `servertype: "openocd"` with
`interface/cmsis-dap.cfg` and the appropriate target configuration, such as
`target/stm32f4x.cfg`. Select your project's ELF file and start at 1000 kHz.

For **pyOCD**:

```powershell
py -m pip install pyocd
pyocd list
pyocd gdbserver --target stm32f407vg --frequency 1000000
```

Replace the example target/configuration with your actual MCU. Some pyOCD
targets require a CMSIS-Pack. These are configuration examples, not claims that
every target or IDE version has been qualified with Plink.

### Everyday use and troubleshooting

With the wiring complete: **power the board and target → run the tray EXE
→ start debugging**. Deployment and driver installation are not daily steps.
The web interface remains at **http://192.168.2.99:8080**.

| Problem | Check |
|---|---|
| Board not reachable | Ethernet, PC subnet and `ping 192.168.2.99` |
| No debugger or COM ports | USB/IP driver installed; `usbip.exe port` lists the matching pod |
| COM ports visible, IDE cannot find probe | CMSIS-DAP selected, WinUSB on interface 0, IDE supports v2; not ST-LINK/J-Link or a COM-port driver |
| Probe found, target missing | Target power, common ground, SWDIO/SWCLK wiring; try 100 kHz |
| Probe busy | Close other IDE, OpenOCD/pyOCD or programming sessions |
| UART1 corrupt or silent | Native UARTLite service active, `kernel_tty_irq` backend, correct TX/RX and 115200 8N1 |
| Web UI looks outdated | Reload with **Ctrl+F5** |

To disconnect, stop debugging and close serial sessions, then choose **Detach**
in the tray. For manual detachment, first run `usbip.exe port` and detach only
the port associated with the exact Plink URL:

```powershell
& "C:\Program Files\USBip\usbip.exe" port
# Enter the matching local port number; do not assume it is always 1.
$plinkPort = Read-Host "Plink local port number from the listing"
& "C:\Program Files\USBip\usbip.exe" detach -p $plinkPort
```

Exit the tray before a planned board reboot or deployment to prevent
auto-attachment. **Do not reset USB hubs as routine troubleshooting.**

## Validation status

This branch uses **hardware/software ABI 2**, which requires a matched FPGA
bitstream and software release. It is not a drop-in update for ABI 1.

- ABI 2 is deployed on the configured board with native interrupt-driven UART1.
- A three-minute concurrent test captured 362 identical reference/Plink serial
  pulses and 358 matching SWO messages, with zero measured UART errors or SWO drops.
- Automatic native-driver startup was observed after a board restart.
- Full-rate dual-UART throughput, external SWD timing and broader target/IDE
  compatibility remain unqualified.
- An earlier Windows `IRQL_NOT_LESS_OR_EQUAL` crash remains unattributed.

See [live results](docs/native-uart-live-20261119.md),
[HARDENING.md](HARDENING.md), [BUGS.md](BUGS.md) and
[SWO_TESTING.md](SWO_TESTING.md) for evidence and remaining limits.

## Implementation

| Area | ABI 2 implementation |
|---|---|
| USB/IP | Bounded queues, eight DAP replies, endpoint-specific pending reads, single cancellation completion |
| Serial | 64 KiB receive/transmit rings per channel; retained short reads and partial sends; reconnectable local links |
| Ownership | Shared C/Python advisory leases prevent duplicate pod access and overlapping flash/control operations |
| SWD | Two FPGA engines; bounded completion waits; configurable turnaround/data phase; real pin release |
| SWO | 32 KiB synchronous banked BRAM FIFO, atomic four-byte pop, loss/framing counters, independent 1 MiB software ring |
| AXI-Lite | Independent AW/W acceptance and stable responses under backpressure |
| Web | Horizontal 2×12 pinouts, pin controls, dual serial console, firmware flashing and device status |
| Deployment | Verified matched releases, source/artifact hashes, maintenance locks and fail-closed rollback |

The fabric remains at **100 MHz**. Divider configuration is not a measurement
of external signal integrity or a guarantee of a particular cable/target speed.

## Web interface

![Plink web interface](docs/plink-overview.png)

After an approved deployment, open `http://192.168.2.99:8080`.

- J3/J4 match the board connector layout: two horizontal rows of 12,
  odd pins above even pins. Select a contact for its pad and controls.
- Serial supports UART0 and UART1, with selectable line endings and text export.
- Controls are always available; there is no API token or unlock step.
- Missing or stale measurements display as a dash, not a fabricated value.
- Programming uploads are binary-safe, limited to 16 MiB and uniquely named.
  The UI asks for confirmation before starting a target write.

The API itself binds to loopback by default; the installed systemd unit enables
LAN access. Use a trusted isolated LAN, firewall, or HTTPS reverse proxy.
Web controls and USB/IP are unauthenticated trusted-network services.
Foreign browser origins are rejected, and hardware ownership checks remain.
Raw UART bridge ports bind to loopback.

For a safe local mock preview, with **no board access**:

```powershell
python tests/preview_ui.py
```

Open the localhost URL it prints. All writes are rejected in this preview.

The current web-only deployment and rollback are recorded in
[`docs/web-ui-20261119.md`](docs/web-ui-20261119.md). It does not replace or
reload the matched FPGA/probe release.

## Wiring

All target signals are **3.3 V logic**, not 5 V tolerant. Share target ground.
The canonical signal mapping is [`hardware/pinmap.json`](hardware/pinmap.json);
unmapped contacts must not be treated as spare GPIO or assumed power pins.

| Header contact | FPGA pad | Signal |
|---|---|---|
| J3.1 / J3.2 | — | Ground reference |
| J3.3 | P16 | SWCLK · pod 1 |
| J3.4 | P15 | SWDIO · pod 1 |
| J3.5 | T14 | nRST, released as open drain |
| **J3.6** | **T10** | **SWO input ← STM32 PB3** |
| J3.7 / J3.8 | C20 / B20 | UART0 TX / RX, from probe perspective |
| J3.11 / J3.12 | P14 / R14 | UART1 TX / RX, from probe perspective |
| J3.15 | T15 | Auxiliary IO0 |
| J4.3 / J4.4 | V18 / V17 | SWCLK / SWDIO · pod 2 |
| J4.5 / J4.6 | Y16 / T16 | nRST / dedicated trace input contact |
| J4.7 / J4.8 | E17 / D18 | Auxiliary IO8 / IO9 |

**Do not drive T10 as BOOT0 while it is connected to PB3.** Default configuration
reserves it for SWO. STM32 UART bootloader handshakes require explicit rewiring
and board-side configuration; the web UI cannot bypass this guard.
The current DAP capture path uses J3 SWO; a J4 trace contact does not imply a
second independently exposed trace stream.

## Build, test and release

Offline checks:

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
python scripts/check_pinmap.py
```

Run `go test ./...` in `plink-tray`. C transport, DAP and SWO ring tests run on
Linux/WSL; RTL simulation and BRAM synthesis use Vivado. See
[HARDENING.md](HARDENING.md) for commands and dependencies.

Build a new matched release with Vivado:

```powershell
vivado -mode batch -source vivado/build_programmer_bitstream.tcl
```

This recreates the generated project, checks the pin map, snapshots source
hashes and packages `.bit`, `.hwh`, PCAP and manifest into `releases/<timestamp>`.
Packaging rejects source changes made during the build.

Verify without deploying:

```powershell
.\deploy_to_board.ps1 -ReleasePath .\releases\<timestamp>
```

**Deployment is a separate live operation**, requiring explicit approval:

```powershell
.\deploy_to_board.ps1 -ReleasePath .\releases\<timestamp> -AllowLiveHardware
```

It stages/builds before stopping clients, installs under `/opt/plink/releases`,
switches `/opt/plink/current`, and loads only a verified release. MMIO clients
require `/run/plink/fpga-ready` containing `plink-abi-2`.
Failure leaves clients stopped rather than reattaching Windows devices.

The repository-root `t9_programmer_pcap.bin` is a **legacy unmatched artifact**,
not a deployment input. Use only generated release directories.

Windows tray source is in `plink-tray/`. The tray EXE is launched explicitly
for everyday use; source builds and deployment do not attach Windows USB/IP
devices automatically.
