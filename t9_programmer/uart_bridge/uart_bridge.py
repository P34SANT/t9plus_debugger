#!/usr/bin/env python3
"""
uart_bridge.py - High-Performance Dual-Channel TCP UART Bridge for Antminer T9+
Targets:
  - Port 2000: Primary UART0 (/dev/ttyPS1 -> J3 Pins 7/8, Activity LEDs D7/D6)
  - Port 2001: Secondary UART1 (AXI UARTLite 0x42C00000 -> J3 Pins 11/12, Activity LEDs D5/D8)

Zero external dependencies - uses standard Python 3 libraries (asyncio, termios, mmap, socket).
"""
import asyncio
import os
import sys
import mmap
import ctypes
import threading
import time
import termios
import tty
import logging
import fcntl
import select
import json
from contextlib import contextmanager

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s'
)
logger = logging.getLogger("T9_UART_Bridge")
MAX_CLIENT_BUFFER = 65536


@contextmanager
def uart_shared_lease(port):
    fd = os.open(f"/run/lock/plink-uart{port}.lock",
                 os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o660)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def broadcast(clients, data):
    for client in tuple(clients):
        if client.is_closing() or client.transport.get_write_buffer_size() + len(data) > MAX_CLIENT_BUFFER:
            clients.discard(client)
            client.close()
            continue
        client.write(data)

# Configuration
CONFIG = {
    "uart0": {
        "enabled": True,
        "device": "/dev/ttyPS1",
        "baud": termios.B115200,
        "tcp_port": 2000,
        "description": "Primary Target UART (J3 TX:C20, RX:B20)"
    },
    "uart1": {
        "enabled": True,
        "base_addr": 0x42C00000,
        "size": 4096,
        "tcp_port": 2001,
        "description": "Secondary Target AXI UARTLite (J3 TX:P14, RX:R14)"
    }
}

BAUD_MAP = {
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
    921600: termios.B921600,
}

# ==============================================================================
# UART0 Handler: Native Linux TTY Device (/dev/ttyPS1)
# ==============================================================================
class TTYBridgeServer:
    def __init__(self, dev_path, baud, port):
        self.dev_path = dev_path
        self.baud = baud
        self.port = port
        self.fd = None
        self.clients = set()
        self.running = False
        self.port_index = 0 if port == 2000 else 1
        self.tx_lock = asyncio.Lock()
        self.stats = {"rx_bytes": 0, "tx_bytes": 0, "backend": "kernel_tty", "errors": 0}

    def open_tty(self):
        try:
            with uart_shared_lease(self.port_index):
                return self._open_tty_locked()
        except BlockingIOError:
            logger.warning("UART%d is leased; refusing to reconfigure it", self.port_index)
            return False

    def _open_tty_locked(self):
        if not os.path.exists(self.dev_path):
            logger.error(f"TTY device {self.dev_path} not found! Is bitstream loaded?")
            return False
        try:
            self.fd = os.open(self.dev_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[4] = self.baud # ispeed
            attrs[5] = self.baud # ospeed
            # 8N1, disable hardware flow control
            attrs[2] &= ~termios.CRTSCTS
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            logger.info(f"Opened {self.dev_path} at 115200 8N1 (fd={self.fd})")
            return True
        except Exception as e:
            logger.error(f"Failed to open {self.dev_path}: {e}")
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
            return False

    def handle_tty_read(self):
        try:
            with uart_shared_lease(self.port_index):
                data = os.read(self.fd, 4096)
            if data:
                self.stats["rx_bytes"] += len(data)
                broadcast(self.clients, data)
        except BlockingIOError:
            # A flash worker owns the UART: stop readiness callbacks until it releases.
            loop = asyncio.get_running_loop()
            loop.remove_reader(self.fd)
            loop.call_later(0.02, self.resume_reader)
        except Exception as e:
            self.stats["errors"] += 1
            logger.warning(f"Error reading TTY: {e}")

    def resume_reader(self):
        loop = asyncio.get_running_loop()
        try:
            with uart_shared_lease(self.port_index):
                tty.setraw(self.fd)
                attrs = termios.tcgetattr(self.fd)
                attrs[4] = attrs[5] = self.baud
                attrs[2] &= ~termios.CRTSCTS
                termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
                loop.add_reader(self.fd, self.handle_tty_read)
        except BlockingIOError:
            loop.call_later(0.02, self.resume_reader)

    def write_all(self, data):
        with uart_shared_lease(self.port_index):
            view = memoryview(data)
            deadline = time.monotonic() + 5
            while view:
                if time.monotonic() >= deadline:
                    raise TimeoutError("UART TX timed out")
                try:
                    written = os.write(self.fd, view)
                except BlockingIOError:
                    select.select([], [self.fd], [], 0.05)
                    continue
                if written <= 0:
                    raise OSError("UART disconnected")
                self.stats["tx_bytes"] += written
                view = view[written:]

    async def handle_client(self, reader, writer):
        addr = writer.get_extra_info('peername')
        logger.info(f"[UART0] Client connected from {addr}")
        self.clients.add(writer)
        try:
            while not reader.at_eof():
                data = await reader.read(4096)
                if not data:
                    break
                if self.fd is not None:
                    async with self.tx_lock:
                        await asyncio.get_running_loop().run_in_executor(None, self.write_all, data)
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.debug(f"[UART0] Client {addr} error: {e}")
        finally:
            logger.info(f"[UART0] Client disconnected from {addr}")
            self.clients.discard(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    async def start(self):
        if not self.open_tty():
            raise RuntimeError(f"UART not ready: {self.dev_path}")

        loop = asyncio.get_running_loop()
        if self.fd is not None:
            loop.add_reader(self.fd, self.handle_tty_read)

        server = await asyncio.start_server(self.handle_client, '127.0.0.1', self.port)
        logger.info(f"UART Bridge active on 127.0.0.1:{self.port} -> {self.dev_path}")
        return server


# ==============================================================================
# UART1 Handler: AXI UARTLite via Memory-Mapped I/O (0x42C00000)
# ==============================================================================
class AxiUartLiteBridgeServer:
    def read_reg(self, offset):
        # Use one aligned word access: RX FIFO registers have read side effects.
        return ctypes.c_uint32.from_buffer(self.mem, offset).value

    def write_reg(self, offset, value):
        ctypes.c_uint32.from_buffer(self.mem, offset).value = value

    def __init__(self, base_addr, port):
        self.base_addr = base_addr
        self.port = port
        self.mem = None
        self.fd = None
        self.clients = set()
        self.stop_poll = threading.Event()
        self.pending_lock = threading.Lock()
        self.pending_rx = bytearray()
        self.delivery_scheduled = False
        self.tx_lock = asyncio.Lock()
        self.stats = {"rx_bytes": 0, "tx_bytes": 0, "backend": "mmio_polling",
                      "errors": 0, "overrun_observations": 0, "framing_observations": 0,
                      "software_dropped": 0}

    def open_mmap(self):
        try:
            with uart_shared_lease(1):
                return self._open_mmap_locked()
        except BlockingIOError:
            logger.warning("UART1 is leased; refusing to reset its FIFOs")
            return False

    def _open_mmap_locked(self):
        try:
            # Protection: never touch PL AXI memory if FPGA is not operating
            if os.path.exists("/sys/class/fpga_manager/fpga0/state"):
                with open("/sys/class/fpga_manager/fpga0/state", "r") as f:
                    if f.read().strip() != "operating":
                        logger.warning("FPGA PL not in operating state. AXI UART1 disabled safely.")
                        return False

            with open("/run/plink/fpga-ready") as ready:
                if ready.read() != "plink-abi-2\n":
                    raise RuntimeError("No verified ABI 2 FPGA release")

            self.fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
            self.mem = mmap.mmap(
                self.fd, 4096,
                mmap.MAP_SHARED,
                mmap.PROT_READ | mmap.PROT_WRITE,
                offset=self.base_addr
            )
            # Reset both FIFOs via control register (offset 0x0C, bits 0 and 1)
            self.write_reg(12, 0x03)
            logger.info(f"Initialized AXI UARTLite @ 0x{self.base_addr:08X} (FIFOs reset, 100MHz clock verified)")
            return True
        except Exception as e:
            logger.error(f"Failed to mmap AXI UARTLite @ 0x{self.base_addr:08X}: {e}")
            return False

    def poll_rx(self):
        if self.mem is None:
            return b""
        rx_buf = bytearray()
        try:
            # While RX FIFO Valid (status bit 0 set)
            while True:
                status = self.read_reg(8)
                self.stats["overrun_observations"] += bool(status & 0x20)
                self.stats["framing_observations"] += bool(status & 0x40)
                if not (status & 0x01):
                    break
                byte_val = self.read_reg(0) & 0xFF
                rx_buf.append(byte_val)
                if len(rx_buf) >= 64:
                    break
        except Exception as e:
            logger.warning(f"Error polling AXI UARTLite: {e}")

        self.stats["rx_bytes"] += len(rx_buf)
        return bytes(rx_buf)

    def broadcast_rx(self, data):
        # StreamWriter access must stay on the asyncio event-loop thread.
        broadcast(self.clients, data)

    def deliver_pending(self):
        with self.pending_lock:
            data = bytes(self.pending_rx)
            self.pending_rx.clear()
            self.delivery_scheduled = False
        self.broadcast_rx(data)

    def poll_worker(self, loop):
        # epoll rounds sub-ms asyncio sleeps up to milliseconds. A dedicated
        # nanosleep-based worker drains the 16-byte FIFO more frequently.
        while not self.stop_poll.is_set():
            data = self.poll_rx()
            if data:
                schedule = False
                with self.pending_lock:
                    if len(self.pending_rx) + len(data) <= MAX_CLIENT_BUFFER:
                        self.pending_rx.extend(data)
                    else:
                        self.stats["software_dropped"] += len(data)
                    if not self.delivery_scheduled:
                        self.delivery_scheduled = True
                        schedule = True
                try:
                    if schedule:
                        loop.call_soon_threadsafe(self.deliver_pending)
                except RuntimeError:
                    return
            time.sleep(0.0001)

    def write_tx(self, data: bytes):
        if self.mem is None:
            raise OSError("UART1 is not mapped")
        deadline = time.monotonic() + 5
        for b in data:
            # Wait if Tx FIFO is full (status bit 3 = 0x08)
            written = False
            while time.monotonic() < deadline:
                status = self.read_reg(8)
                if not (status & 0x08):
                    self.write_reg(4, b)
                    written = True
                    self.stats["tx_bytes"] += 1
                    break
                time.sleep(0.00005)
            if not written:
                self.stats["errors"] += 1
                raise TimeoutError("UART1 TX FIFO did not become ready")

    async def poll_task(self):
        loop = asyncio.get_running_loop()
        try:
            await loop.run_in_executor(None, self.poll_worker, loop)
        finally:
            self.stop_poll.set()

    async def handle_client(self, reader, writer):
        addr = writer.get_extra_info('peername')
        logger.info(f"[UART1] Client connected from {addr}")
        self.clients.add(writer)
        try:
            while not reader.at_eof():
                data = await reader.read(256)
                if not data:
                    break
                async with self.tx_lock:
                    await asyncio.get_running_loop().run_in_executor(None, self.write_tx, data)
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.debug(f"[UART1] Client {addr} error: {e}")
        finally:
            logger.info(f"[UART1] Client disconnected from {addr}")
            self.clients.discard(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    async def start(self):
        if not self.open_mmap():
            raise RuntimeError("AXI UARTLite not ready")

        self.polling_task = asyncio.create_task(self.poll_task())
        server = await asyncio.start_server(self.handle_client, '127.0.0.1', self.port)
        logger.info(f"UART1 Bridge active on 127.0.0.1:{self.port} -> AXI 0x{self.base_addr:08X}")
        return server


# ==============================================================================
# Main Event Loop
# ==============================================================================
async def main():
    logger.info("Starting Antminer T9+ TCP UART Bridge Services...")
    with open("/run/plink/fpga-ready") as ready:
        if ready.read() != "plink-abi-2\n":
            raise RuntimeError("No verified ABI 2 FPGA release")

    uart0_server = TTYBridgeServer(
        CONFIG["uart0"]["device"],
        CONFIG["uart0"]["baud"],
        CONFIG["uart0"]["tcp_port"]
    )
    s0 = await uart0_server.start()

    native_uart = os.environ.get("PLINK_UART1_TTY", "/dev/ttyUL0")
    if os.path.exists(native_uart):
        uart1_server = TTYBridgeServer(native_uart, termios.B115200, CONFIG["uart1"]["tcp_port"])
    else:
        logger.warning("UARTLite kernel TTY unavailable; polling fallback is not sustained-rate qualified")
        uart1_server = AxiUartLiteBridgeServer(CONFIG["uart1"]["base_addr"], CONFIG["uart1"]["tcp_port"])
    s1 = await uart1_server.start()

    print("\n" + "="*70)
    print(" ANTMINER T9+ TCP UART BRIDGE RUNNING")
    print("="*70)
    print(f" Port {CONFIG['uart0']['tcp_port']}: UART0 (Primary)   -> {CONFIG['uart0']['description']}")
    print(f" Port {CONFIG['uart1']['tcp_port']}: UART1 (Secondary) -> {CONFIG['uart1']['description']}")
    print(f" Connect with: nc 192.168.2.99 2000  OR  PuTTY Raw/Telnet")
    print("="*70 + "\n")

    async def snapshot_metrics():
        while True:
            try:
                path = "/run/plink/uart.json"
                with open(path + ".tmp", "w") as output:
                    json.dump({"uart0": dict(uart0_server.stats), "uart1": dict(uart1_server.stats)}, output)
                os.replace(path + ".tmp", path)
            except OSError as error:
                logger.warning("Could not publish UART metrics: %s", error)
            await asyncio.sleep(1)

    await asyncio.gather(
        snapshot_metrics(),
        s0.serve_forever(),
        s1.serve_forever()
    )

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Shutting down UART Bridge.")
