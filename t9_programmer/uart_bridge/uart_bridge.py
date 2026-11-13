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
import struct
import mmap
import termios
import tty
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s'
)
logger = logging.getLogger("T9_UART_Bridge")

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

    def open_tty(self):
        if not os.path.exists(self.dev_path):
            logger.error(f"TTY device {self.dev_path} not found! Is bitstream loaded?")
            return False
        try:
            self.fd = os.open(self.dev_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(self.fd)
            tty.setraw(self.fd)
            attrs[4] = self.baud # ispeed
            attrs[5] = self.baud # ospeed
            # 8N1, disable hardware flow control
            attrs[2] &= ~termios.CRTSCTS
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            logger.info(f"Opened {self.dev_path} at 115200 8N1 (fd={self.fd})")
            return True
        except Exception as e:
            logger.error(f"Failed to open {self.dev_path}: {e}")
            return False

    def handle_tty_read(self):
        try:
            data = os.read(self.fd, 4096)
            if data:
                disconnected = set()
                for client in self.clients:
                    try:
                        client.write(data)
                    except Exception:
                        disconnected.add(client)
                self.clients -= disconnected
        except BlockingIOError:
            pass
        except Exception as e:
            logger.warning(f"Error reading TTY: {e}")

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
                    try:
                        os.write(self.fd, data)
                    except Exception as e:
                        logger.warning(f"Error writing to TTY: {e}")
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
            logger.warning(f"Could not open {self.dev_path} immediately, will retry in background.")

        loop = asyncio.get_running_loop()
        if self.fd is not None:
            loop.add_reader(self.fd, self.handle_tty_read)

        server = await asyncio.start_server(self.handle_client, '0.0.0.0', self.port)
        logger.info(f"UART0 Bridge active on 0.0.0.0:{self.port} -> {self.dev_path}")
        return server


# ==============================================================================
# UART1 Handler: AXI UARTLite via Memory-Mapped I/O (0x42C00000)
# ==============================================================================
class AxiUartLiteBridgeServer:
    def __init__(self, base_addr, port):
        self.base_addr = base_addr
        self.port = port
        self.mem = None
        self.fd = None
        self.clients = set()

    def open_mmap(self):
        try:
            self.fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
            self.mem = mmap.mmap(
                self.fd, 4096,
                mmap.MAP_SHARED,
                mmap.PROT_READ | mmap.PROT_WRITE,
                offset=self.base_addr
            )
            # Reset both FIFOs via control register (offset 0x0C, bits 0 and 1)
            self.mem[12:16] = struct.pack('<I', 0x03)
            logger.info(f"Initialized AXI UARTLite @ 0x{self.base_addr:08X} (FIFOs reset)")
            return True
        except Exception as e:
            logger.error(f"Failed to mmap AXI UARTLite @ 0x{self.base_addr:08X}: {e}")
            return False

    def poll_rx(self):
        if self.mem is None or not self.clients:
            return
        rx_buf = bytearray()
        try:
            # While RX FIFO Valid (status bit 0 set)
            while True:
                status = struct.unpack('<I', self.mem[8:12])[0]
                if not (status & 0x01):
                    break
                byte_val = struct.unpack('<I', self.mem[0:4])[0] & 0xFF
                rx_buf.append(byte_val)
                if len(rx_buf) >= 64:
                    break
        except Exception as e:
            logger.warning(f"Error polling AXI UARTLite: {e}")

        if rx_buf:
            disconnected = set()
            for client in self.clients:
                try:
                    client.write(bytes(rx_buf))
                except Exception:
                    disconnected.add(client)
            self.clients -= disconnected

    def write_tx(self, data: bytes):
        if self.mem is None:
            return
        for b in data:
            # Wait if Tx FIFO is full (status bit 3 = 0x08)
            for _ in range(1000):
                status = struct.unpack('<I', self.mem[8:12])[0]
                if not (status & 0x08):
                    break
            self.mem[4:8] = struct.pack('<I', b)

    async def poll_task(self):
        while True:
            if not self.clients:
                await asyncio.sleep(0.05)
                continue
            self.poll_rx()
            await asyncio.sleep(0.002) # 2ms polling loop

    async def handle_client(self, reader, writer):
        addr = writer.get_extra_info('peername')
        logger.info(f"[UART1] Client connected from {addr}")
        self.clients.add(writer)
        try:
            while not reader.at_eof():
                data = await reader.read(256)
                if not data:
                    break
                self.write_tx(data)
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
            logger.warning(f"Could not mmap AXI UARTLite, retrying...")

        asyncio.create_task(self.poll_task())
        server = await asyncio.start_server(self.handle_client, '0.0.0.0', self.port)
        logger.info(f"UART1 Bridge active on 0.0.0.0:{self.port} -> AXI 0x{self.base_addr:08X}")
        return server


# ==============================================================================
# Main Event Loop
# ==============================================================================
async def main():
    logger.info("Starting Antminer T9+ TCP UART Bridge Services...")

    uart0_server = TTYBridgeServer(
        CONFIG["uart0"]["device"],
        CONFIG["uart0"]["baud"],
        CONFIG["uart0"]["tcp_port"]
    )
    s0 = await uart0_server.start()

    uart1_server = AxiUartLiteBridgeServer(
        CONFIG["uart1"]["base_addr"],
        CONFIG["uart1"]["tcp_port"]
    )
    s1 = await uart1_server.start()

    print("\n" + "="*70)
    print(" ANTMINER T9+ TCP UART BRIDGE RUNNING")
    print("="*70)
    print(f" Port {CONFIG['uart0']['tcp_port']}: UART0 (Primary)   -> {CONFIG['uart0']['description']}")
    print(f" Port {CONFIG['uart1']['tcp_port']}: UART1 (Secondary) -> {CONFIG['uart1']['description']}")
    print(f" Connect with: nc 192.168.2.99 2000  OR  PuTTY Raw/Telnet")
    print("="*70 + "\n")

    await asyncio.gather(
        s0.serve_forever(),
        s1.serve_forever()
    )

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("Shutting down UART Bridge.")
