#!/usr/bin/env python3
"""
io_daemon.py - Antminer T9+ Target I/O Controller & AI Automation REST Server
Exposes:
  - Configurable 3.3V Target I/Os with AI Semantic Roles & Aliases (J3/J4)
  - Hardware Reset / Bootloader automation endpoints
  - Bicolor Status LED D3 (Red: MIO37, Green: MIO38)
  - Web UI Dashboard & REST API on port 8080
  - Target Firmware Flash helper via OpenOCD

Zero external dependencies - standard Python 3.10 library only.
"""
import os
import sys
import json
import time
import threading
import subprocess
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s'
)
logger = logging.getLogger("T9_IO_Daemon")

CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pins.json")
PORT = 8080

# Hardware LED pins (PS MIO)
GPIO_D3_RED = 549   # MIO 37
GPIO_D3_GREEN = 550 # MIO 38

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
            except Exception as e:
                logger.debug(f"Export GPIO {gpio_num}: {e}")

    @staticmethod
    def unexport(gpio_num):
        path = f"/sys/class/gpio/gpio{gpio_num}"
        if os.path.exists(path):
            try:
                with open("/sys/class/gpio/unexport", "w") as f:
                    f.write(str(gpio_num))
            except Exception as e:
                logger.debug(f"Unexport GPIO {gpio_num}: {e}")

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
        except Exception as e:
            logger.error(f"Read GPIO {gpio_num}: {e}")
        finally:
            if unexport_after:
                SysfsGPIO.unexport(gpio_num)
        return val


# ==============================================================================
# Bicolor LED D3 Controller (Red: MIO37, Green: MIO38)
# ==============================================================================
class D3LedManager:
    def __init__(self):
        self.mode = "idle" # "idle", "programming", "success", "error", "off"
        self.stop_event = threading.Event()
        self.thread = None
        # Init hardware GPIOs
        SysfsGPIO.set_direction(GPIO_D3_RED, "out")
        SysfsGPIO.set_direction(GPIO_D3_GREEN, "out")
        self.set_mode("idle")

    def _apply_raw(self, red, green):
        SysfsGPIO.write(GPIO_D3_RED, red)
        SysfsGPIO.write(GPIO_D3_GREEN, green)

    def set_mode(self, mode):
        self.mode = mode
        logger.info(f"D3 LED Status -> '{mode}'")
        if self.thread and self.thread.is_alive():
            self.stop_event.set()
            self.thread.join(timeout=1.0)
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._run_pattern, daemon=True)
        self.thread.start()

    def _run_pattern(self):
        if self.mode == "idle":
            # Solid Green
            self._apply_raw(0, 1)
        elif self.mode == "off":
            self._apply_raw(0, 0)
        elif self.mode == "error":
            # Solid Red
            self._apply_raw(1, 0)
        elif self.mode == "programming":
            # Fast alternating Red and Green (100ms)
            while not self.stop_event.is_set():
                self._apply_raw(1, 0)
                if self.stop_event.wait(0.1): break
                self._apply_raw(0, 1)
                if self.stop_event.wait(0.1): break
        elif self.mode == "success":
            # Triple rapid green pulse, then solid green
            for _ in range(3):
                self._apply_raw(0, 1)
                time.sleep(0.1)
                self._apply_raw(0, 0)
                time.sleep(0.1)
            self._apply_raw(0, 1)
            self.mode = "idle"


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
                logger.warning(f"Config file {self.config_file} not found, initializing empty.")
                self.pins = {}

    def save_config(self):
        with self.lock:
            with open(self.config_file, "w") as f:
                json.dump(self.pins, f, indent=2)

    def apply_initial_states(self):
        for name, pin in self.pins.items():
            # If pin is shared with OpenOCD (e.g. target_nrst), do not hold sysfs export
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
        # 1. Exact key match
        if query in self.pins:
            return query, self.pins[query]
        # 2. Alias match
        for key, pin in self.pins.items():
            if pin.get("alias", "").lower() == query:
                return key, pin
        # 3. Role match
        for key, pin in self.pins.items():
            if pin.get("role", "").lower() == query:
                return key, pin
        return None, None

    def set_value(self, query, value):
        key, pin = self.resolve_pin(query)
        if not pin:
            raise KeyError(f"Pin '{query}' not found")
        gpio = pin["sysfs_gpio"]
        int_val = 1 if int(value) else 0
        SysfsGPIO.set_direction(gpio, "out")
        SysfsGPIO.write(gpio, int_val)
        if pin.get("shared_swd"):
            SysfsGPIO.unexport(gpio)
        pin["direction"] = "out"
        pin["current_val"] = int_val
        self.save_config()
        return key, pin

    def get_value(self, query):
        key, pin = self.resolve_pin(query)
        if not pin:
            raise KeyError(f"Pin '{query}' not found")
        gpio = pin["sysfs_gpio"]
        val = SysfsGPIO.read(gpio, unexport_after=pin.get("shared_swd", False))
        pin["current_val"] = val
        return key, val

    def pulse(self, query, duration_ms=50, active_low=None):
        key, pin = self.resolve_pin(query)
        if not pin:
            raise KeyError(f"Pin '{query}' not found")
        gpio = pin["sysfs_gpio"]
        if active_low is None:
            active_low = pin.get("active_low", False)

        SysfsGPIO.set_direction(gpio, "out")
        # Assert pulse
        assert_val = 0 if active_low else 1
        deassert_val = 1 if active_low else 0

        SysfsGPIO.write(gpio, assert_val)
        time.sleep(duration_ms / 1000.0)
        SysfsGPIO.write(gpio, deassert_val)
        if pin.get("shared_swd"):
            SysfsGPIO.unexport(gpio)
        pin["current_val"] = deassert_val
        self.save_config()
        return key, pin

    def configure_pin(self, key, alias=None, role=None, direction=None, description=None, active_low=None):
        real_key, pin = self.resolve_pin(key)
        if not pin:
            raise KeyError(f"Pin '{key}' not found")
        if alias is not None: pin["alias"] = alias.strip()
        if role is not None: pin["role"] = role.strip()
        if description is not None: pin["description"] = description.strip()
        if active_low is not None: pin["active_low"] = bool(active_low)
        if direction is not None:
            direction = direction.lower()
            if direction in ["in", "out"]:
                pin["direction"] = direction
                SysfsGPIO.set_direction(pin["sysfs_gpio"], direction)
        self.save_config()
        return real_key, pin


# Global Singletons
d3_manager = D3LedManager()
pin_manager = PinManager(CONFIG_PATH)

# ==============================================================================
# HTML Web Dashboard
# ==============================================================================
WEB_DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Antminer T9+ STM32 Programmer & I/O Control</title>
<style>
  :root {
    --bg: #0f172a;
    --card: #1e293b;
    --border: #334155;
    --primary: #38bdf8;
    --green: #22c55e;
    --red: #ef4444;
    --text: #f8fafc;
    --text-muted: #94a3b8;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    padding: 24px;
  }
  .container { max-width: 1200px; margin: 0 auto; }
  header {
    display: flex; justify-content: space-between; align-items: center;
    border-bottom: 1px solid var(--border); padding-bottom: 16px; margin-bottom: 24px;
  }
  h1 { font-size: 1.5rem; color: var(--primary); }
  .badge { background: #0369a1; padding: 4px 10px; border-radius: 9999px; font-size: 0.8rem; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 20px; }
  .card {
    background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 20px;
    box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3);
  }
  .card h2 { font-size: 1.15rem; margin-bottom: 14px; border-bottom: 1px solid var(--border); padding-bottom: 8px; }
  .btn {
    background: #0284c7; color: #fff; border: none; padding: 8px 14px; border-radius: 6px;
    font-weight: 600; cursor: pointer; transition: background 0.15s;
  }
  .btn:hover { background: #0369a1; }
  .btn-red { background: var(--red); }
  .btn-red:hover { background: #dc2626; }
  .btn-green { background: var(--green); }
  .btn-green:hover { background: #16a34a; }
  .btn-sm { padding: 4px 8px; font-size: 0.8rem; }
  table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 0.9rem; }
  th, td { text-align: left; padding: 8px; border-bottom: 1px solid var(--border); }
  th { color: var(--text-muted); font-weight: 600; }
  .status-dot {
    display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px;
  }
  .dot-green { background: var(--green); box-shadow: 0 0 8px var(--green); }
  .dot-red { background: var(--red); box-shadow: 0 0 8px var(--red); }
  .input-text {
    background: #0f172a; border: 1px solid var(--border); color: #fff;
    padding: 6px 10px; border-radius: 6px; width: 100%; font-size: 0.85rem;
  }
  .flex-row { display: flex; gap: 8px; align-items: center; }
</style>
</head>
<body>
<div class="container">
  <header>
    <div>
      <h1>Antminer T9+ STM32 Programmer Pod</h1>
      <p style="color:var(--text-muted); font-size:0.9rem;">Xilinx Zynq XC7Z010 &bull; Dual UART &bull; 10x 3.3V Target I/Os &bull; OpenOCD SWD</p>
    </div>
    <div class="flex-row">
      <span class="badge" id="board-ip">192.168.2.99</span>
      <span class="badge" style="background:#15803d;" id="fpga-status">PL: OPERATING</span>
    </div>
  </header>

  <div class="grid">
    <!-- Target Control & Automation Card -->
    <div class="card">
      <h2>Target Quick Actions (AI Automation)</h2>
      <p style="color:var(--text-muted); font-size:0.85rem; margin-bottom:14px;">
        High-level commands accessible via REST API for automated STM32 programming:
      </p>
      <div style="display:flex; flex-direction:column; gap:12px;">
        <div style="display:flex; justify-content:space-between; align-items:center;">
          <div>
            <strong>Target Hard Reset (nRST)</strong>
            <div style="font-size:0.8rem; color:var(--text-muted);">Pulses J3 Pin 5 Low for 50ms</div>
          </div>
          <button class="btn btn-red" onclick="pulsePin('rst', 50)">Reset Target</button>
        </div>

        <div style="display:flex; justify-content:space-between; align-items:center;">
          <div>
            <strong>STM32 Bootloader Mode</strong>
            <div style="font-size:0.8rem; color:var(--text-muted);">Assert BOOT0 & pulse Reset</div>
          </div>
          <button class="btn" onclick="enterBootloader()">Enter Bootloader</button>
        </div>

        <div style="display:flex; justify-content:space-between; align-items:center;">
          <div>
            <strong>D3 Programming LED Mode</strong>
            <div style="font-size:0.8rem; color:var(--text-muted);" id="led-mode-text">Current: idle</div>
          </div>
          <div class="flex-row">
            <button class="btn btn-sm btn-green" onclick="setLed('idle')">Idle</button>
            <button class="btn btn-sm btn-red" onclick="setLed('programming')">Flash</button>
          </div>
        </div>
      </div>
    </div>

    <!-- Ethernet Services Status Card -->
    <div class="card">
      <h2>Turnkey Ethernet Services</h2>
      <table>
        <tr>
          <th>Service</th>
          <th>Port</th>
          <th>Interface</th>
        </tr>
        <tr>
          <td><strong>OpenOCD GDB</strong></td>
          <td><code>3333</code></td>
          <td>SWD (J3 SWCLK, SWDIO, nRST)</td>
        </tr>
        <tr>
          <td><strong>OpenOCD Telnet</strong></td>
          <td><code>4444</code></td>
          <td>Telnet interactive console</td>
        </tr>
        <tr>
          <td><strong>Primary UART0</strong></td>
          <td><code>2000</code></td>
          <td>J3 Pin 7 (TX), Pin 8 (RX) [LED7/D6]</td>
        </tr>
        <tr>
          <td><strong>Secondary UART1</strong></td>
          <td><code>2001</code></td>
          <td>J3 Pin 11 (TX), Pin 12 (RX) [D5/LED8]</td>
        </tr>
        <tr>
          <td><strong>REST / AI API</strong></td>
          <td><code>8080</code></td>
          <td>HTTP JSON Control API</td>
        </tr>
      </table>
    </div>
  </div>

  <!-- Configurable Pins Table -->
  <div class="card" style="margin-top:24px;">
    <h2>Configurable 3.3V Target I/O Pins (Headers J3 & J4)</h2>
    <table>
      <thead>
        <tr>
          <th>Pin Name</th>
          <th>Header / FPGA</th>
          <th>AI Alias</th>
          <th>Assigned Role</th>
          <th>Dir</th>
          <th>Value</th>
          <th>Actions</th>
        </tr>
      </thead>
      <tbody id="pins-table-body">
        <tr><td colspan="7">Loading pin states...</td></tr>
      </tbody>
    </table>
  </div>
</div>

<script>
async function fetchPins() {
  try {
    const res = await fetch('/api/pins');
    const data = await res.json();
    const tbody = document.getElementById('pins-table-body');
    tbody.innerHTML = '';

    for (const [key, pin] of Object.entries(data)) {
      const isHigh = pin.current_val === 1;
      const dotClass = isHigh ? 'dot-green' : 'dot-red';

      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td><strong>${key}</strong><br><small style="color:var(--text-muted);">${pin.description || ''}</small></td>
        <td><code>${pin.header} Pin ${pin.pin_num} (${pin.fpga_pin})</code></td>
        <td><input type="text" class="input-text" style="width:110px;" value="${pin.alias || ''}" onchange="updatePinConfig('${key}', 'alias', this.value)"></td>
        <td><input type="text" class="input-text" style="width:120px;" value="${pin.role || ''}" onchange="updatePinConfig('${key}', 'role', this.value)"></td>
        <td>
          <button class="btn btn-sm" onclick="toggleDir('${key}', '${pin.direction}')">${pin.direction.toUpperCase()}</button>
        </td>
        <td><span class="status-dot ${dotClass}"></span>${pin.current_val}</td>
        <td>
          <div class="flex-row">
            <button class="btn btn-sm ${isHigh ? 'btn-red' : 'btn-green'}" onclick="setPin('${key}', ${isHigh ? 0 : 1})">
              ${isHigh ? 'Set 0' : 'Set 1'}
            </button>
            <button class="btn btn-sm" onclick="pulsePin('${key}', 50)">Pulse 50ms</button>
          </div>
        </td>
      `;
      tbody.appendChild(tr);
    }
  } catch (e) {
    console.error("Failed to load pins:", e);
  }
}

async function setPin(query, val) {
  await fetch(`/api/pins/${query}/set`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ val: val })
  });
  fetchPins();
}

async function pulsePin(query, ms) {
  await fetch(`/api/pins/${query}/pulse`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ms: ms })
  });
  fetchPins();
}

async function toggleDir(key, currentDir) {
  const newDir = currentDir === 'out' ? 'in' : 'out';
  await updatePinConfig(key, 'direction', newDir);
  fetchPins();
}

async function updatePinConfig(key, field, val) {
  const payload = { pin: key };
  payload[field] = val;
  await fetch('/api/pins/configure', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
}

async function enterBootloader() {
  await fetch('/api/programmer/bootloader', { method: 'POST' });
  alert("STM32 Target put into BOOTLOADER mode (BOOT0=1, Reset pulsed).");
  fetchPins();
}

async function setLed(mode) {
  await fetch(`/api/led/status?mode=${mode}`, { method: 'POST' });
  document.getElementById('led-mode-text').innerText = 'Current: ' + mode;
}

fetchPins();
setInterval(fetchPins, 3000);
</script>
</body>
</html>
"""

# ==============================================================================
# REST API Request Handler
# ==============================================================================
class RestApiHandler(BaseHTTPRequestHandler):
    def send_json(self, data, status=200):
        body = json.dumps(data, indent=2).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
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
            body = WEB_DASHBOARD_HTML.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        elif path == "/api/status":
            state = "unknown"
            if os.path.exists("/sys/class/fpga_manager/fpga0/state"):
                with open("/sys/class/fpga_manager/fpga0/state", "r") as f:
                    state = f.read().strip()
            self.send_json({
                "status": "ok",
                "fpga_state": state,
                "d3_led_mode": d3_manager.mode,
                "ports": {
                    "openocd_gdb": 3333,
                    "openocd_telnet": 4444,
                    "openocd_tcl": 6666,
                    "uart0_tcp": 2000,
                    "uart1_tcp": 2001,
                    "rest_api": 8080
                }
            })
            return

        elif path == "/api/pins":
            # Refresh current values
            res = {}
            for k, pin in pin_manager.pins.items():
                pin["current_val"] = SysfsGPIO.read(pin["sysfs_gpio"], unexport_after=pin.get("shared_swd", False))
                res[k] = pin
            self.send_json(res)
            return

        elif path.startswith("/api/pins/") and path.endswith("/get"):
            parts = path.split('/')
            query = parts[3]
            try:
                key, val = pin_manager.get_value(query)
                self.send_json({"pin": key, "val": val})
            except KeyError as e:
                self.send_json({"error": str(e)}, 404)
            return

        self.send_json({"error": f"Endpoint not found: {self.path}"}, 404)

    def do_POST(self):
        url = urlparse(self.path)
        path = url.path.rstrip('/')
        content_len = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_len).decode('utf-8') if content_len > 0 else "{}"
        try:
            payload = json.loads(body) if body else {}
        except Exception:
            payload = {}

        if path == "/api/pins/configure":
            pin_name = payload.get("pin")
            if not pin_name:
                self.send_json({"error": "Missing 'pin' parameter"}, 400)
                return
            try:
                key, updated = pin_manager.configure_pin(
                    pin_name,
                    alias=payload.get("alias"),
                    role=payload.get("role"),
                    direction=payload.get("direction"),
                    description=payload.get("description"),
                    active_low=payload.get("active_low")
                )
                self.send_json({"status": "ok", "pin": key, "config": updated})
            except KeyError as e:
                self.send_json({"error": str(e)}, 404)
            return

        elif path.startswith("/api/pins/") and path.endswith("/set"):
            parts = path.split('/')
            query = parts[3]
            val = payload.get("val", 1)
            try:
                key, pin = pin_manager.set_value(query, val)
                self.send_json({"status": "ok", "pin": key, "val": pin["current_val"]})
            except KeyError as e:
                self.send_json({"error": str(e)}, 404)
            return

        elif path.startswith("/api/pins/") and path.endswith("/pulse"):
            parts = path.split('/')
            query = parts[3]
            ms = payload.get("ms", 50)
            active_low = payload.get("active_low")
            try:
                key, pin = pin_manager.pulse(query, ms, active_low)
                self.send_json({"status": "ok", "pin": key, "pulsed_ms": ms})
            except KeyError as e:
                self.send_json({"error": str(e)}, 404)
            return

        elif path == "/api/programmer/reset":
            # Pulse hardware reset
            try:
                pin_manager.pulse("rst", 50, active_low=True)
                d3_manager.set_mode("idle")
                self.send_json({"status": "ok", "action": "target_reset"})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        elif path == "/api/programmer/bootloader":
            # Assert BOOT0=1, pulse reset, leave BOOT0=1
            try:
                pin_manager.set_value("boot0", 1)
                pin_manager.pulse("rst", 50, active_low=True)
                self.send_json({"status": "ok", "action": "enter_bootloader", "boot0": 1})
            except Exception as e:
                self.send_json({"error": str(e)}, 500)
            return

        elif path == "/api/led/status":
            qs = parse_qs(url.query)
            mode = qs.get("mode", ["idle"])[0]
            d3_manager.set_mode(mode)
            self.send_json({"status": "ok", "led_mode": mode})
            return

        elif path == "/api/programmer/flash":
            # Trigger OpenOCD Flash
            firmware_path = payload.get("firmware_path")
            target = payload.get("target", "stm32f1x")
            if not firmware_path or not os.path.isfile(firmware_path):
                self.send_json({"error": f"Firmware file not found: {firmware_path}"}, 400)
                return

            def flash_worker():
                d3_manager.set_mode("programming")
                # Temporarily pause background OpenOCD daemon if active to avoid SWD contention
                was_active = False
                check_svc = subprocess.run(["systemctl", "is-active", "t9-openocd.service"], capture_output=True, text=True)
                if check_svc.returncode == 0 and check_svc.stdout.strip() == "active":
                    was_active = True
                    subprocess.run(["systemctl", "stop", "t9-openocd.service"])
                    time.sleep(0.5)

                cmd = [
                    "openocd",
                    "-f", "/home/xilinx/t9_programmer/openocd/t9_swd.cfg",
                    "-f", f"target/{target}.cfg",
                    "-c", "init",
                    "-c", "reset init",
                    "-c", f"program {firmware_path} verify reset exit"
                ]
                res = subprocess.run(cmd, capture_output=True, text=True)
                if res.returncode == 0:
                    d3_manager.set_mode("success")
                else:
                    d3_manager.set_mode("error")

                if was_active:
                    subprocess.run(["systemctl", "start", "t9-openocd.service"])

            threading.Thread(target=flash_worker, daemon=True).start()
            self.send_json({"status": "started", "message": f"Flashing {firmware_path} on {target}..."})
            return

        self.send_json({"error": f"Endpoint not found: {self.path}"}, 404)


def run_server():
    server_address = ('0.0.0.0', PORT)
    httpd = HTTPServer(server_address, RestApiHandler)
    logger.info(f"Target I/O Daemon & Web Dashboard running on http://0.0.0.0:{PORT}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        logger.info("Stopping I/O Daemon.")
    finally:
        httpd.server_close()

if __name__ == '__main__':
    run_server()
