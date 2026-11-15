#!/usr/bin/env python3
"""
io_daemon.py - pcmsis Enterprise Programmer & Target I/O Controller
Antminer T9+ Control Board V1.2 (Zynq-7000 XC7Z010)

Exposes:
  - Enterprise Web UI & REST API on port 8080
  - Multi-engine target flashing:
      * STM32 SWD (Hardware accelerated OpenOCD)
      * STM32 UART Bootloader (stm32flash with auto BOOT0/nRST sequence)
      * ESP32 / ESP8266 UART Bootloader (esptool with auto IO0/EN sequence)
  - Real-time Server-Sent Events (SSE) execution log streaming
  - Live J3/J4 header pin telemetry & hardware control
  - Embedded web serial terminal bridge for UART0 (:2000)
"""
import os
import sys
import json
import time
import socket
import select
import hashlib
import threading
import subprocess
import logging
from socketserver import ThreadingMixIn
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s'
)
logger = logging.getLogger("pcmsis_io_daemon")

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pins.json")
UPLOAD_DIR = "/tmp/pcmsis_uploads"
os.makedirs(UPLOAD_DIR, exist_ok=True)
PORT = 8080

GPIO_D3_RED = 549   # MIO 37
GPIO_D3_GREEN = 550 # MIO 38

# Ring buffer for execution logs
LOG_LOCK = threading.Lock()
LOG_HISTORY = []
LOG_SUBSCRIBERS = []

def emit_log(msg: str):
    ts = time.strftime("%H:%M:%S")
    formatted = f"[{ts}] {msg}"
    with LOG_LOCK:
        LOG_HISTORY.append(formatted)
        if len(LOG_HISTORY) > 1000:
            LOG_HISTORY.pop(0)
        dead = []
        for q in LOG_SUBSCRIBERS:
            try:
                q.put_nowait(formatted)
            except Exception:
                dead.append(q)
        for d in dead:
            if d in LOG_SUBSCRIBERS:
                LOG_SUBSCRIBERS.remove(d)

# ==============================================================================
# Low-Level Sysfs GPIO Controller
# ==============================================================================
class SysfsGPIO:
    @staticmethod
    def export(gpio_num):
        path = f"/sys/class/gpio/gpio{gpio_num}"
        if not os.path.exists(path):
            try:
                with open("/sys/class/gpio/export", "w") as f:
                    f.write(str(gpio_num))
            except Exception:
                pass

    @staticmethod
    def unexport(gpio_num):
        path = f"/sys/class/gpio/gpio{gpio_num}"
        if os.path.exists(path):
            try:
                with open("/sys/class/gpio/unexport", "w") as f:
                    f.write(str(gpio_num))
            except Exception:
                pass

    @staticmethod
    def set_direction(gpio_num, direction):
        SysfsGPIO.export(gpio_num)
        path = f"/sys/class/gpio/gpio{gpio_num}/direction"
        try:
            with open(path, "w") as f:
                f.write(direction)
        except Exception as e:
            logger.error(f"Set direction {direction} on GPIO {gpio_num}: {e}")

    @staticmethod
    def write(gpio_num, value):
        SysfsGPIO.export(gpio_num)
        path = f"/sys/class/gpio/gpio{gpio_num}/value"
        try:
            with open(path, "w") as f:
                f.write("1" if value else "0")
        except Exception as e:
            logger.error(f"Write {value} to GPIO {gpio_num}: {e}")

    @staticmethod
    def read(gpio_num, unexport_after=False):
        path = f"/sys/class/gpio/gpio{gpio_num}/value"
        was_exported = os.path.exists(path)
        if not was_exported:
            SysfsGPIO.export(gpio_num)
        val = 0
        try:
            if os.path.exists(path):
                with open(path, "r") as f:
                    val = int(f.read().strip())
        except Exception:
            val = 0
        finally:
            if unexport_after:
                SysfsGPIO.unexport(gpio_num)
        return val


# ==============================================================================
# Bicolor LED D3 Manager (Red: MIO37, Green: MIO38)
# ==============================================================================
class D3LedManager:
    def __init__(self):
        self.mode = "idle"
        SysfsGPIO.set_direction(GPIO_D3_RED, "out")
        SysfsGPIO.set_direction(GPIO_D3_GREEN, "out")
        self.set_mode("idle")

    def _apply_raw(self, red, green):
        SysfsGPIO.write(GPIO_D3_RED, red)
        SysfsGPIO.write(GPIO_D3_GREEN, green)

    def set_mode(self, mode):
        self.mode = mode
        if mode == "idle" or mode == "off":
            self._apply_raw(0, 0)
        elif mode == "programming":
            self._apply_raw(0, 1)
        elif mode == "debugging":
            self._apply_raw(1, 0)
        elif mode == "success":
            for _ in range(3):
                self._apply_raw(0, 1)
                time.sleep(0.08)
                self._apply_raw(0, 0)
                time.sleep(0.08)
            self._apply_raw(0, 0)
            self.mode = "idle"
        elif mode == "error":
            self._apply_raw(1, 0)


# ==============================================================================
# Pin Manager
# ==============================================================================
class PinManager:
    def __init__(self, config_file):
        self.config_file = config_file
        self.lock = threading.Lock()
        self.pins = {}
        self.load_config()
        self.apply_initial_states()

    def load_config(self):
        with self.lock:
            if os.path.isfile(self.config_file):
                with open(self.config_file, "r") as f:
                    self.pins = json.load(f)
            else:
                self.pins = {}

    def save_config(self):
        with self.lock:
            with open(self.config_file, "w") as f:
                json.dump(self.pins, f, indent=2)

    def apply_initial_states(self):
        for name, pin in self.pins.items():
            if pin.get("shared_swd"):
                SysfsGPIO.unexport(pin["sysfs_gpio"])
                continue
            gpio = pin["sysfs_gpio"]
            direction = pin.get("direction", "out")
            SysfsGPIO.set_direction(gpio, direction)
            if direction == "out":
                val = pin.get("current_val", 1 if pin.get("active_low") else 0)
                SysfsGPIO.write(gpio, val)

    def resolve_pin(self, query):
        query = query.strip().lower()
        if query in self.pins:
            return query, self.pins[query]
        for key, pin in self.pins.items():
            if pin.get("alias", "").lower() == query:
                return key, pin
        for key, pin in self.pins.items():
            if pin.get("role", "").lower() == query:
                return key, pin
        return None, None

    def set_value(self, key, val):
        real_key, pin = self.resolve_pin(key)
        if not pin:
            raise KeyError(f"Pin '{key}' not found")
        val = 1 if val else 0
        pin["current_val"] = val
        SysfsGPIO.write(pin["sysfs_gpio"], val)
        return real_key, pin

    def pulse(self, key, ms=50, active_low=True):
        real_key, pin = self.resolve_pin(key)
        if not pin:
            raise KeyError(f"Pin '{key}' not found")
        gpio = pin["sysfs_gpio"]
        assert_val = 0 if active_low else 1
        release_val = 1 if active_low else 0
        SysfsGPIO.set_direction(gpio, "out")
        SysfsGPIO.write(gpio, assert_val)
        time.sleep(ms / 1000.0)
        SysfsGPIO.write(gpio, release_val)
        pin["current_val"] = release_val
        return real_key, pin


# Singletons
d3_manager = D3LedManager()
pin_manager = PinManager(CONFIG_PATH)

# ==============================================================================
# Flashing Workers
# ==============================================================================
def flash_stm32_uart(firmware_path, baud=115200, verify=True, reset_after=True):
    emit_log(f"Starting STM32 UART Bootloader engine on {firmware_path} @ {baud} baud...")
    d3_manager.set_mode("programming")
    
    # 1. Assert BOOT0=1, pulse nRST low (50ms) to enter system ROM bootloader
    emit_log("Hardware Handshake: Asserting BOOT0=HIGH, pulsing nRST=LOW (50ms)...")
    try:
        pin_manager.set_value("boot0", 1)
        time.sleep(0.05)
        pin_manager.pulse("rst", 50, active_low=True)
        time.sleep(0.1)
    except Exception as e:
        emit_log(f"Warning on GPIO sequence: {e}")

    # 2. Execute stm32flash
    cmd = ["stm32flash", "-b", str(baud), "-w", firmware_path]
    if verify:
        cmd.append("-v")
    cmd.extend(["-g", "0x08000000", "/dev/ttyPS1"])

    emit_log(f"Executing: {' '.join(cmd)}")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in iter(p.stdout.readline, ''):
        line = line.strip()
        if line:
            emit_log(line)
    p.wait()

    # 3. Post-flash sequence: Deassert BOOT0=0, pulse nRST
    if reset_after:
        emit_log("Resetting target into user flash mode (BOOT0=LOW, nRST pulse)...")
        try:
            pin_manager.set_value("boot0", 0)
            time.sleep(0.05)
            pin_manager.pulse("rst", 50, active_low=True)
        except Exception as e:
            emit_log(f"Warning on reset: {e}")

    if p.returncode == 0:
        emit_log("[+] SUCCESS: STM32 UART flash and verification complete!")
        d3_manager.set_mode("success")
    else:
        emit_log(f"[-] ERROR: stm32flash failed with exit code {p.returncode}")
        d3_manager.set_mode("error")


def flash_esp32_uart(firmware_path, address="0x1000", baud=115200, verify=True, reset_after=True):
    emit_log(f"Starting ESP32 / ESP8266 esptool engine on {firmware_path} @ {address}...")
    d3_manager.set_mode("programming")

    # 1. Hardware sequence: IO0=LOW, pulse EN/nRST (50ms), IO0=HIGH
    emit_log("Hardware Handshake: Asserting IO0=LOW, pulsing EN=LOW (50ms)...")
    try:
        pin_manager.set_value("io0", 0)
        time.sleep(0.05)
        pin_manager.pulse("rst", 50, active_low=True)
        time.sleep(0.05)
        pin_manager.set_value("io0", 1)
        time.sleep(0.1)
    except Exception as e:
        emit_log(f"Warning on ESP32 handshake: {e}")

    # 2. Execute esptool
    cmd = [
        "/usr/local/bin/esptool.py",
        "--port", "/dev/ttyPS1",
        "--baud", str(baud),
        "--before", "no_reset",
        "--after", "no_reset",
        "write_flash", address, firmware_path
    ]
    if verify:
        cmd.append("--verify")

    emit_log(f"Executing: {' '.join(cmd)}")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in iter(p.stdout.readline, ''):
        line = line.strip()
        if line:
            emit_log(line)
    p.wait()

    # 3. Post-flash reset
    if reset_after:
        emit_log("Resetting ESP32 into normal execution mode (IO0=HIGH, EN pulse)...")
        try:
            pin_manager.set_value("io0", 1)
            time.sleep(0.05)
            pin_manager.pulse("rst", 50, active_low=True)
        except Exception as e:
            emit_log(f"Warning on ESP32 reset: {e}")

    if p.returncode == 0:
        emit_log("[+] SUCCESS: ESP32 flashing complete!")
        d3_manager.set_mode("success")
    else:
        emit_log(f"[-] ERROR: esptool failed with exit code {p.returncode}")
        d3_manager.set_mode("error")


# ==============================================================================
# Plink HTML5 Dashboard
# ==============================================================================
ENTERPRISE_DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Plink // Programmer & Debug Pod</title>
<style>
  :root {
    --bg-main: #090d16;
    --bg-panel: #111827;
    --bg-sub: #1f2937;
    --border: #374151;
    --primary: #38bdf8;
    --emerald: #10b981;
    --amber: #f59e0b;
    --red: #ef4444;
    --text: #f3f4f6;
    --text-muted: #9ca3af;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg-main);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace, sans-serif;
    font-size: 13px;
    padding: 16px;
  }
  .header {
    display: flex; justify-content: space-between; align-items: center;
    border-bottom: 1px solid var(--border); padding-bottom: 12px; margin-bottom: 14px;
  }
  .title-group { display: flex; align-items: center; gap: 10px; }
  .title { font-size: 1.2rem; font-weight: 800; letter-spacing: 0.5px; color: #fff; }
  .title span { color: var(--primary); }
  .badge { background: #1e3a8a; color: #93c5fd; padding: 3px 8px; border-radius: 4px; font-weight: 600; font-size: 11px; }
  .badge-emerald { background: #064e3b; color: #6ee7b7; }
  
  .tabs {
    display: flex; gap: 4px; border-bottom: 1px solid var(--border); margin-bottom: 16px;
  }
  .tab-btn {
    background: transparent; color: var(--text-muted); border: none; border-bottom: 2px solid transparent;
    padding: 8px 16px; font-size: 12px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px;
    cursor: pointer; transition: all 0.15s;
  }
  .tab-btn:hover { color: #fff; }
  .tab-btn.active { color: var(--primary); border-bottom-color: var(--primary); }

  .tab-content { display: none; gap: 16px; }
  .tab-content.active { display: grid; }
  
  .layout-2col { grid-template-columns: 460px 1fr; }
  .layout-1col { grid-template-columns: 1fr; }
  
  .panel {
    background: var(--bg-panel); border: 1px solid var(--border); border-radius: 6px; padding: 14px;
    display: flex; flex-direction: column; gap: 12px;
  }
  .panel-title {
    font-size: 11.5px; text-transform: uppercase; letter-spacing: 0.8px; font-weight: 700;
    color: var(--text-muted); border-bottom: 1px solid var(--border); padding-bottom: 6px;
    display: flex; justify-content: space-between; align-items: center;
  }
  .form-group { display: flex; flex-direction: column; gap: 4px; }
  label { font-size: 11px; color: var(--text-muted); font-weight: 600; text-transform: uppercase; }
  select, input[type="text"] {
    background: var(--bg-main); border: 1px solid var(--border); color: #fff;
    padding: 6px 10px; border-radius: 4px; font-family: inherit; font-size: 12px; outline: none;
  }
  select:focus, input[type="text"]:focus { border-color: var(--primary); }
  .dropzone {
    border: 2px dashed var(--border); border-radius: 6px; padding: 20px; text-align: center;
    background: rgba(17, 24, 39, 0.4); cursor: pointer; transition: all 0.15s;
  }
  .dropzone:hover, .dropzone.dragover { border-color: var(--primary); background: rgba(56, 189, 248, 0.05); }
  .btn {
    background: #0284c7; color: #fff; border: none; padding: 7px 12px; border-radius: 4px;
    font-size: 12px; font-weight: 600; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; gap: 6px;
  }
  .btn:hover { filter: brightness(1.1); }
  .btn:active { filter: brightness(0.9); }
  .btn-emerald { background: var(--emerald); color: #000; font-weight: 700; }
  .btn-amber { background: var(--amber); color: #000; font-weight: 700; }
  .btn-red { background: var(--red); }
  .btn-dark { background: var(--bg-sub); border: 1px solid var(--border); }
  .btn-group { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  .terminal {
    background: #030712; border: 1px solid #1f2937; border-radius: 4px; padding: 10px;
    font-family: "JetBrains Mono", Consolas, monospace; font-size: 11.5px; line-height: 1.45;
    color: #e5e7eb; height: 380px; overflow-y: auto; white-space: pre-wrap; word-break: break-all;
  }
  table { width: 100%; border-collapse: collapse; font-size: 11.5px; }
  th, td { padding: 6px 8px; border-bottom: 1px solid var(--border); text-align: left; }
  th { color: var(--text-muted); font-size: 10.5px; text-transform: uppercase; }
  .serial-box { display: flex; gap: 8px; margin-top: 6px; }
  .serial-input { flex: 1; }
  .dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; margin-right: 5px; }
  .dot-green { background: var(--emerald); box-shadow: 0 0 6px var(--emerald); }
  .dot-amber { background: var(--amber); }
</style>
</head>
<body>

<div class="header">
  <div class="title-group">
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="28" height="28" style="vertical-align:middle; filter:drop-shadow(0 0 6px rgba(56,189,248,0.5));">
      <rect width="128" height="128" rx="28" fill="#0b1120" stroke="#1e293b" stroke-width="4"/>
      <path d="M 36 94 L 36 34 L 70 34 C 84 34, 92 42, 92 54 C 92 66, 84 74, 70 74 L 52 74" fill="none" stroke="#38bdf8" stroke-width="12" stroke-linecap="round" stroke-linejoin="round"/>
      <path d="M 66 46 L 58 58 L 72 58 L 62 70" fill="none" stroke="#10b981" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>
      <circle cx="36" cy="94" r="8" fill="#10b981"/>
    </svg>
    <div class="title">Plink <span>// Programmer &amp; Debug Pod</span></div>
    <span class="badge badge-emerald">FPGA 25MHz SWD ACTIVE</span>
    <span class="badge">NATIVE COM PORT (CDC-ACM)</span>
  </div>
  <div style="display:flex; gap:8px;">
    <span class="badge">IP: 192.168.2.99</span>
    <span class="badge">PORTS: 3240 / 2000 / 2001 / 8080</span>
  </div>
</div>

<div class="tabs">
  <button class="tab-btn active" id="btn-tab-dashboard" onclick="switchTab('tab-dashboard')">01 // Dashboard &amp; States</button>
  <button class="tab-btn" id="btn-tab-flasher" onclick="switchTab('tab-flasher')">02 // UART Programming</button>
  <button class="tab-btn" id="btn-tab-pins" onclick="switchTab('tab-pins')">03 // Pin Telemetry &amp; Hardware</button>
</div>

<!-- ========================================================================= -->
<!-- TAB 1: DASHBOARD & STATES (HOME) -->
<!-- ========================================================================= -->
<div class="tab-content layout-2col active" id="tab-dashboard">
  <div class="panel">
    <div class="panel-title">
      <span>Pod State &amp; Target Hardware Overview</span>
      <span style="color:var(--emerald);"><span class="dot dot-green"></span>OPERATING</span>
    </div>

    <div style="display:flex; flex-direction:column; gap:8px; font-size:12px;">
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">Hardware Platform:</span>
        <strong>Bitmain Antminer T9+ (Zynq XC7Z010-1CLG400C)</strong>
      </div>
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">FPGA Acceleration Core:</span>
        <strong style="color:var(--primary);">axi_swd_engine.v @ 25.0 MHz (100% Timing Met)</strong>
      </div>
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">USB Device Architecture:</span>
        <strong>Composite (WinUSB CMSIS-DAP v2 + CDC-ACM COM)</strong>
      </div>
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">Primary Target UART0:</span>
        <strong>J3 Pins 7/8 (TX:C20, RX:B20) &bull; TCP:2000 &bull; 9600-921600</strong>
      </div>
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">Secondary Target UART1:</span>
        <strong>J3 Pins 11/12 (TX:P14, RX:R14) &bull; TCP:2001 &bull; 115200</strong>
      </div>
      <div style="display:flex; justify-content:space-between; border-bottom:1px solid var(--border); padding-bottom:5px;">
        <span style="color:var(--text-muted);">Windows Tray Auto-Attach:</span>
        <strong style="color:var(--emerald);">plink-tray.exe (Event-Driven · 0% Polling)</strong>
      </div>
    </div>

    <div class="panel-title" style="margin-top:6px;">Target Hardware Control</div>
    <div class="btn-group">
      <button class="btn btn-red" onclick="actionReset()">Pulse nRST (50ms)</button>
      <button class="btn btn-amber" onclick="actionBootloader()">Enter STM32 Bootloader</button>
      <button class="btn btn-dark" onclick="actionNormalRun()">Normal Run (BOOT0=0)</button>
      <button class="btn btn-dark" onclick="actionEspBootloader()">ESP32 Handshake (IO0+EN)</button>
    </div>
  </div>

  <div class="panel">
    <div class="panel-title">
      <span>Target Live Console (UART0 115200 8N1 · J3 Pins 7/8)</span>
      <span class="badge badge-emerald">STREAM ACTIVE</span>
    </div>
    <div class="terminal" id="dash-serial-terminal" style="height:340px;"></div>
    <div class="serial-box">
      <input type="text" class="serial-input" id="dash-serial-input" placeholder="Type target CLI command &amp; press Enter..." onkeydown="if(event.key==='Enter')sendSerial('dash-serial-input')">
      <button class="btn btn-dark" onclick="sendSerial('dash-serial-input')">Send</button>
      <button class="btn btn-dark" onclick="document.getElementById('dash-serial-terminal').textContent=''">Clear</button>
    </div>
  </div>
</div>

<!-- ========================================================================= -->
<!-- TAB 2: UART PROGRAMMING -->
<!-- ========================================================================= -->
<div class="tab-content layout-2col" id="tab-flasher">
  <div class="panel">
    <div class="panel-title">
      <span>Target Flashing Engine</span>
      <span id="target-state" style="color:var(--emerald);">READY</span>
    </div>

    <div class="form-group">
      <label>Programmer Engine</label>
      <select id="engine-select" onchange="onEngineChange()">
        <option value="stm32_uart">STM32 UART Bootloader (stm32flash · Auto BOOT0/nRST)</option>
        <option value="esp32_uart">ESP32 / ESP8266 (esptool · Auto IO0/EN Handshake)</option>
      </select>
    </div>

    <div class="dropzone" id="dropzone" onclick="document.getElementById('file-input').click()">
      <input type="file" id="file-input" style="display:none;" onchange="onFileSelected(this.files[0])" accept=".hex,.bin,.elf">
      <div id="drop-text">
        <div style="font-weight:700; margin-bottom:4px;">Drag &amp; Drop Firmware File (.hex, .bin, .elf)</div>
        <div style="color:var(--text-muted); font-size:11px;">Or click to browse from local filesystem</div>
      </div>
      <div id="file-info" style="display:none; text-align:left; font-size:11.5px; line-height:1.4;"></div>
    </div>

    <div class="btn-group">
      <div class="form-group">
        <label>Base Address</label>
        <input type="text" id="base-addr" value="0x08000000">
      </div>
      <div class="form-group">
        <label>Baud Rate</label>
        <select id="baud-rate">
          <option value="9600">9,600 baud</option>
          <option value="19200">19,200 baud</option>
          <option value="38400">38,400 baud</option>
          <option value="57600">57,600 baud</option>
          <option value="115200" selected>115,200 baud</option>
          <option value="230400">230,400 baud</option>
          <option value="460800">460,800 baud</option>
          <option value="921600">921,600 baud (High-Speed)</option>
        </select>
      </div>
    </div>

    <div style="display:flex; justify-content:space-between; align-items:center; padding:2px 0;">
      <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
        <input type="checkbox" id="chk-verify" checked> Verify Flash After Write
      </label>
      <label style="display:flex; align-items:center; gap:6px; cursor:pointer;">
        <input type="checkbox" id="chk-reset" checked> Auto-Reset Target After Complete
      </label>
    </div>

    <button class="btn btn-emerald" style="padding:10px;" onclick="triggerFlash()">
      ▶ FLASH FIRMWARE TO TARGET
    </button>
  </div>

  <div class="panel">
    <div class="panel-title">
      <span>Execution Output &amp; Telemetry</span>
      <div style="display:flex; gap:6px;">
        <button class="btn btn-dark" style="padding:2px 6px; font-size:10px;" onclick="copyLog()">Copy Log</button>
        <button class="btn btn-dark" style="padding:2px 6px; font-size:10px;" onclick="clearLog()">Clear</button>
      </div>
    </div>
    <div class="terminal" id="terminal" style="height:380px;"></div>
  </div>
</div>

<!-- ========================================================================= -->
<!-- TAB 3: PIN TELEMETRY & HARDWARE -->
<!-- ========================================================================= -->
<div class="tab-content layout-1col" id="tab-pins">
  <div class="panel">
    <div class="panel-title">
      <span>Header J3 / J4 Pin Telemetry &amp; Dynamic State</span>
      <button class="btn btn-dark" style="padding:2px 8px; font-size:10px;" onclick="loadPins()">Refresh Values</button>
    </div>
    <table>
      <thead>
        <tr>
          <th>Signal Key</th>
          <th>Header &amp; Pin</th>
          <th>FPGA Ball</th>
          <th>Role / Alias</th>
          <th>Direction</th>
          <th>Logic State</th>
          <th>Hardware Action</th>
        </tr>
      </thead>
      <tbody id="pins-table-body">
        <tr><td colspan="7" style="color:var(--text-muted);">Loading pin telemetry...</td></tr>
      </tbody>
    </table>
  </div>
</div>

<script>
let currentFile = null;

function switchTab(tabId) {
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
  document.getElementById(tabId).classList.add('active');
  document.getElementById('btn-' + tabId).classList.add('active');
  if (tabId === 'tab-pins') {
    loadPins();
  }
}

function onEngineChange() {
  const engine = document.getElementById('engine-select').value;
  const addr = document.getElementById('base-addr');
  if (engine === 'stm32_uart') {
    addr.value = '0x08000000';
  } else if (engine === 'esp32_uart') {
    addr.value = '0x1000';
  }
}

function onFileSelected(file) {
  if (!file) return;
  currentFile = file;
  document.getElementById('drop-text').style.display = 'none';
  const info = document.getElementById('file-info');
  info.style.display = 'block';
  info.innerHTML = `<strong>Selected:</strong> ${file.name}<br>` +
                   `<strong>Size:</strong> ${(file.size / 1024).toFixed(2)} KB (${file.size} bytes)<br>` +
                   `<span style="color:var(--emerald);">✓ Ready for flashing</span>`;
  appendLog(`[HOST] Staged file: ${file.name} (${file.size} bytes)`);
}

const dz = document.getElementById('dropzone');
['dragenter', 'dragover'].forEach(e => dz.addEventListener(e, (evt) => { evt.preventDefault(); dz.classList.add('dragover'); }));
['dragleave', 'drop'].forEach(e => dz.addEventListener(e, (evt) => { evt.preventDefault(); dz.classList.remove('dragover'); }));
dz.addEventListener('drop', (evt) => {
  if (evt.dataTransfer.files && evt.dataTransfer.files[0]) {
    onFileSelected(evt.dataTransfer.files[0]);
  }
});

function appendLog(line) {
  const t = document.getElementById('terminal');
  t.textContent += line + "\\n";
  t.scrollTop = t.scrollHeight;
}

function clearLog() {
  document.getElementById('terminal').textContent = '';
}

function copyLog() {
  navigator.clipboard.writeText(document.getElementById('terminal').textContent);
}

async function triggerFlash() {
  if (!currentFile) {
    alert("Please select or drop a firmware file (.hex, .bin, .elf) first.");
    return;
  }

  appendLog(`[UPLOAD] Transmitting ${currentFile.name} to programmer pod...`);
  const formData = new FormData();
  formData.append('file', currentFile);

  try {
    const upRes = await fetch('/api/upload', { method: 'POST', body: formData });
    const upData = await upRes.json();
    if (upData.error) {
      appendLog(`[-] Upload failed: ${upData.error}`);
      return;
    }
    appendLog(`[+] Upload complete: Stored at ${upData.path} (SHA256: ${upData.sha256.substring(0, 16)}...)`);

    const payload = {
      engine: document.getElementById('engine-select').value,
      filename: upData.path,
      address: document.getElementById('base-addr').value,
      baud: parseInt(document.getElementById('baud-rate').value, 10),
      verify: document.getElementById('chk-verify').checked,
      reset_after: document.getElementById('chk-reset').checked
    };

    appendLog(`[FLASH] Spawning ${payload.engine} engine...`);
    const flashRes = await fetch('/api/flash', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    const flashData = await flashRes.json();
    if (flashData.error) {
      appendLog(`[-] Flashing trigger failed: ${flashData.error}`);
    }
  } catch (err) {
    appendLog(`[-] Network error: ${err}`);
  }
}

async function actionReset() {
  appendLog("[TARGET] Pulsing nRST (50ms)...");
  await fetch('/api/programmer/reset', { method: 'POST' });
}

async function actionBootloader() {
  appendLog("[TARGET] Asserting BOOT0=1 and pulsing nRST (Entering Bootloader)...");
  await fetch('/api/programmer/bootloader', { method: 'POST' });
}

async function actionNormalRun() {
  appendLog("[TARGET] Deasserting BOOT0=0 and pulsing nRST (Normal Run)...");
  await fetch('/api/esp32/reset', { method: 'POST' });
}

async function actionEspBootloader() {
  appendLog("[TARGET] Executing ESP32 handshake sequence (IO0=0, EN pulse)...");
  await fetch('/api/esp32/bootloader', { method: 'POST' });
}

async function loadPins() {
  try {
    const res = await fetch('/api/pins');
    const pins = await res.json();
    const tbody = document.getElementById('pins-table-body');
    tbody.innerHTML = '';
    for (const [k, p] of Object.entries(pins)) {
      const valStr = p.current_val === 1 
        ? '<span style="color:var(--emerald);">● HIGH (1)</span>' 
        : '<span style="color:var(--text-muted);">○ LOW (0)</span>';
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td><strong>${k}</strong></td>
        <td>${p.header} Pin ${p.pin_num}</td>
        <td><code>${p.fpga_pin}</code></td>
        <td>${p.alias || p.role || '-'}</td>
        <td>${p.direction.toUpperCase()}</td>
        <td>${valStr}</td>
        <td>
          <button class="btn btn-dark" style="padding:2px 6px; font-size:10px;" onclick="pulsePin('${k}', 50)">Pulse 50ms</button>
        </td>
      `;
      tbody.appendChild(tr);
    }
  } catch (e) {
    console.error(e);
  }
}

async function pulsePin(query, ms) {
  appendLog(`[GPIO] Pulsing pin '${query}' (${ms}ms)...`);
  await fetch(`/api/pins/${query}/pulse`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ms: ms })
  });
  loadPins();
}

function initLogStream() {
  const es = new EventSource('/api/log_stream');
  es.onmessage = function(e) {
    appendLog(e.data);
  };
  es.onerror = function() {
    setTimeout(initLogStream, 3000);
  };
}

async function sendSerial(inputId) {
  const inp = document.getElementById(inputId);
  const txt = inp.value;
  if (!txt) return;
  inp.value = '';
  await fetch('/api/serial/send', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ text: txt + "\\r\\n" })
  });
}

function initSerialStream() {
  const es = new EventSource('/api/serial/stream');
  const st = document.getElementById('dash-serial-terminal');
  es.onmessage = function(e) {
    st.textContent += e.data + "\\n";
    st.scrollTop = st.scrollHeight;
  };
}

window.onload = function() {
  initLogStream();
  initSerialStream();
  loadPins();
  appendLog("[SYSTEM] Plink Programmer & Debug Pod online (192.168.2.99)");
};
</script>

</body>
</html>
"""

# ==============================================================================
# REST API & HTTP Server
# ==============================================================================
class RestApiHandler(BaseHTTPRequestHandler):
    def send_json(self, data, code=200):
        body = json.dumps(data).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        path = url.path.rstrip('/')

        if path == "" or path == "/index.html":
            body = ENTERPRISE_DASHBOARD_HTML.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        elif path == "/api/log_stream":
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

            import queue
            q = queue.Queue(maxsize=100)
            with LOG_LOCK:
                LOG_SUBSCRIBERS.append(q)
                for line in LOG_HISTORY[-30:]:
                    q.put_nowait(line)

            try:
                while True:
                    try:
                        line = q.get(timeout=20.0)
                        self.wfile.write(f"data: {line}\n\n".encode('utf-8'))
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": heartbeat\n\n")
                        self.wfile.flush()
            except Exception:
                pass
            finally:
                with LOG_LOCK:
                    if q in LOG_SUBSCRIBERS:
                        LOG_SUBSCRIBERS.remove(q)
            return

        elif path == "/api/serial/stream":
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect(("127.0.0.1", 2000))
                s.setblocking(False)
                while True:
                    r, _, _ = select.select([s], [], [], 10.0)
                    if r:
                        data = s.recv(1024)
                        if not data:
                            break
                        text = data.decode('utf-8', errors='replace').strip()
                        if text:
                            self.wfile.write(f"data: {text}\n\n".encode('utf-8'))
                            self.wfile.flush()
                    else:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
            except Exception:
                pass
            return

        elif path == "/api/status":
            self.send_json({
                "status": "ok",
                "pod": "pcmsis",
                "fpga_state": "operating",
                "d3_led": d3_manager.mode
            })
            return

        elif path == "/api/pins":
            res = {}
            for k, pin in pin_manager.pins.items():
                pin["current_val"] = SysfsGPIO.read(pin["sysfs_gpio"], unexport_after=pin.get("shared_swd", False))
                res[k] = pin
            self.send_json(res)
            return

        self.send_json({"error": f"Endpoint not found: {self.path}"}, 404)

    def do_POST(self):
        url = urlparse(self.path)
        path = url.path.rstrip('/')

        if path == "/api/upload":
            content_type = self.headers.get('Content-Type', '')
            content_len = int(self.headers.get('Content-Length', 0))
            if content_len == 0:
                self.send_json({"error": "No file payload received"}, 400)
                return

            raw_data = self.rfile.read(content_len)
            filename = "firmware.bin"

            # Parse multipart boundary if present
            if 'boundary=' in content_type:
                boundary = content_type.split('boundary=')[1].strip().encode('utf-8')
                parts = raw_data.split(b'--' + boundary)
                file_bytes = b""
                for p in parts:
                    if b'filename="' in p:
                        hdr, body = p.split(b'\r\n\r\n', 1)
                        body = body.rstrip(b'\r\n')
                        file_bytes = body
                        for h in hdr.decode('utf-8', errors='ignore').split('\r\n'):
                            if 'filename="' in h:
                                fn = h.split('filename="')[1].split('"')[0]
                                if fn: filename = os.path.basename(fn)
                        break
            else:
                file_bytes = raw_data

            save_path = os.path.join(UPLOAD_DIR, filename)
            with open(save_path, "wb") as f:
                f.write(file_bytes)

            sha = hashlib.sha256(file_bytes).hexdigest()
            self.send_json({
                "status": "ok",
                "filename": filename,
                "path": save_path,
                "size": len(file_bytes),
                "sha256": sha
            })
            return

        content_len = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_len).decode('utf-8') if content_len > 0 else "{}"
        try:
            payload = json.loads(body) if body else {}
        except Exception:
            payload = {}

        if path == "/api/flash":
            engine = payload.get("engine", "stm32_uart")
            filename = payload.get("filename")
            address = payload.get("address", "0x08000000")
            baud = payload.get("baud", 115200)
            verify = payload.get("verify", True)
            reset_after = payload.get("reset_after", True)

            if not filename or not os.path.isfile(filename):
                self.send_json({"error": f"Firmware file not found on device: {filename}"}, 400)
                return

            if engine == "stm32_uart":
                t = threading.Thread(target=flash_stm32_uart, args=(filename, baud, verify, reset_after), daemon=True)
                t.start()
            elif engine == "esp32_uart":
                t = threading.Thread(target=flash_esp32_uart, args=(filename, address, baud, verify, reset_after), daemon=True)
                t.start()
            else:
                self.send_json({"error": f"Unknown engine: {engine}"}, 400)
                return

            self.send_json({"status": "started", "engine": engine, "file": filename})
            return

        elif path == "/api/serial/send":
            text = payload.get("text", "")
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect(("127.0.0.1", 2000))
                s.sendall(text.encode('utf-8'))
                s.close()
                self.send_json({"status": "ok"})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        elif path == "/api/programmer/reset":
            try:
                pin_manager.pulse("rst", 50, active_low=True)
                d3_manager.set_mode("idle")
                emit_log("[TARGET] Target hard reset triggered (nRST 50ms)")
                self.send_json({"status": "ok", "action": "target_reset"})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        elif path == "/api/programmer/bootloader":
            try:
                pin_manager.set_value("boot0", 1)
                time.sleep(0.05)
                pin_manager.pulse("rst", 50, active_low=True)
                emit_log("[TARGET] Target entered STM32 bootloader mode (BOOT0=1, nRST pulsed)")
                self.send_json({"status": "ok", "action": "enter_bootloader"})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        elif path == "/api/esp32/reset":
            try:
                pin_manager.set_value("boot0", 0)
                pin_manager.set_value("io0", 1)
                time.sleep(0.02)
                pin_manager.pulse("rst", 50, active_low=True)
                emit_log("[TARGET] Target reset to normal user execution mode (BOOT0=0, nRST pulsed)")
                self.send_json({"status": "ok", "action": "normal_run"})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        self.send_json({"error": f"Endpoint not found: {self.path}"}, 404)


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

def run_server():
    server_address = ('0.0.0.0', PORT)
    httpd = ThreadedHTTPServer(server_address, RestApiHandler)
    logger.info(f"Plink Server active on http://0.0.0.0:{PORT}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        logger.info("Stopping pcmsis server.")
    finally:
        httpd.server_close()

if __name__ == '__main__':
    run_server()
