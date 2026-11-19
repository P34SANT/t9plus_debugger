#!/usr/bin/env python3
"""Run the immutable probe bridge with a required native TTY and kernel counters."""
import asyncio
import importlib.util
import struct
from pathlib import Path

import activate


def load_bridge():
    source = Path("/opt/plink/current/t9_programmer/uart_bridge/uart_bridge.py")
    spec = importlib.util.spec_from_file_location("plink_verified_bridge", source)
    bridge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge)
    return bridge


def native_class(bridge):
    class NativeTTYBridge(bridge.TTYBridgeServer):
        def counters(self):
            data = bytearray(80)  # Linux serial_icounter_struct: twenty 32-bit ints.
            try:
                bridge.fcntl.ioctl(self.fd, 0x545D, data, True)  # TIOCGICOUNT
                values = struct.unpack("=20i", data)
                for key, index in (("kernel_rx", 4), ("kernel_tx", 5),
                                   ("framing_errors", 6), ("hardware_overruns", 7),
                                   ("parity_errors", 8), ("breaks", 9),
                                   ("kernel_buffer_overruns", 10)):
                    self.stats[key] = values[index] & 0xFFFFFFFF
                self.stats["counter_state"] = "measured"
            except OSError:
                self.stats["counter_state"] = "unavailable"

        def _open_tty_locked(self):
            result = super()._open_tty_locked()
            if result:
                if self.port_index == 1:
                    self.stats["backend"] = "kernel_tty_irq"
                self.counters()
            return result

        def handle_tty_read(self):
            super().handle_tty_read()
            self.counters()

        def write_all(self, data):
            super().write_all(data)
            self.counters()
    return NativeTTYBridge


def main():
    activate.verify_package()
    activate.verify_native_tty()
    bridge = load_bridge()
    # A missing native TTY is fatal, not permission to resume MMIO polling.
    bridge.TTYBridgeServer = native_class(bridge)
    bridge.os.environ["PLINK_UART1_TTY"] = "/dev/ttyUL0"
    asyncio.run(bridge.main())


if __name__ == "__main__":
    main()
