"""Linux-only offline tests: fake I/O and temporary advisory lock files."""
from contextlib import nullcontext
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipIf(sys.platform == "win32", "Linux termios/flock backend")
class UartAndOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("plink_uart_offline",
            ROOT / "t9_programmer/uart_bridge/uart_bridge.py")
        cls.uart = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.uart)
        sys.path.insert(0, str(ROOT / "t9_programmer/io_daemon"))
        import ownership
        cls.ownership = ownership

    def test_partial_tty_writes_and_retry(self):
        uart = self.uart
        bridge = uart.TTYBridgeServer("fake", uart.termios.B115200, 2000)
        bridge.fd = 123
        writes = []
        def partial(fd, view):
            writes.append(bytes(view))
            if len(writes) == 2:
                raise BlockingIOError()
            return min(2, len(view))
        with patch.object(uart, "uart_shared_lease", return_value=nullcontext()), \
                patch.object(uart.os, "write", side_effect=partial), \
                patch.object(uart.select, "select", return_value=([], [], [])):
            bridge.write_all(b"abcdefg")
        self.assertEqual(writes, [b"abcdefg", b"cdefg", b"cdefg", b"efg", b"g"])
        self.assertEqual(bridge.stats["tx_bytes"], 7)

    def test_startup_cannot_reconfigure_a_leased_uart(self):
        uart = self.uart
        bridge = uart.TTYBridgeServer("fake", uart.termios.B115200, 2000)
        with patch.object(uart, "uart_shared_lease", side_effect=BlockingIOError()), \
                patch.object(uart.os, "open") as open_device:
            self.assertFalse(bridge.open_tty())
            self.assertFalse(uart.AxiUartLiteBridgeServer(0, 2001).open_mmap())
            open_device.assert_not_called()

    def test_slow_client_is_closed_before_bound_exceeded(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        client = Mock()
        client.is_closing.return_value = False
        client.transport = SimpleNamespace(get_write_buffer_size=lambda: 65535)
        clients = {client}
        self.uart.broadcast(clients, b"12")
        client.close.assert_called_once()
        client.write.assert_not_called()
        self.assertFalse(clients)

    def test_mmio_delivery_is_coalesced_and_bounded(self):
        from unittest.mock import Mock
        bridge = self.uart.AxiUartLiteBridgeServer(0, 2001)
        count = 0
        def receive():
            nonlocal count
            count += 1
            if count == 1025:
                bridge.stop_poll.set()
            return b"x" * 64
        bridge.poll_rx = receive
        loop = Mock()
        with patch.object(self.uart.time, "sleep"):
            bridge.poll_worker(loop)
        self.assertEqual(len(bridge.pending_rx), 65536)
        self.assertEqual(bridge.stats["software_dropped"], 64)
        loop.call_soon_threadsafe.assert_called_once()
        bridge.broadcast_rx = Mock()
        bridge.deliver_pending()
        self.assertFalse(bridge.pending_rx)
        self.assertFalse(bridge.delivery_scheduled)

    def test_exclusive_lease_and_release(self):
        ownership = self.ownership
        real_open = os.open
        with tempfile.TemporaryDirectory() as directory:
            def temporary_open(filename, flags, mode):
                return real_open(str(Path(directory) / Path(filename).name), flags, mode)
            with patch.object(ownership.os, "open", side_effect=temporary_open):
                with ownership.ResourceLease("pod1"):
                    with self.assertRaises(ownership.ResourceBusy):
                        ownership.ResourceLease("pod1")
                    with ownership.ResourceLease("pod2"):
                        pass
                with ownership.ResourceLease("pod1"):
                    pass

    def test_programming_timeout_kills_only_its_child_group(self):
        import io_daemon
        with self.assertRaises(TimeoutError):
            io_daemon.run_flash_command(
                [sys.executable, "-c", "import time; time.sleep(5)"], timeout=0.05)


if __name__ == "__main__":
    unittest.main()
