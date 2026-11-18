"""Offline tests only: USB and serial access are replaced with mocks."""
from contextlib import redirect_stderr
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import swo_dap_test
import swo_serial_test
import swo_target_regs


class FakeDevice:
    def __init__(self, reply):
        self.reply = reply
        self.writes = []

    def write(self, endpoint, data, timeout):
        self.writes.append((endpoint, data))

    def read(self, endpoint, length, timeout):
        return self.reply


class SwoToolsTests(unittest.TestCase):
    def test_decodes_port_zero_source_packets(self):
        self.assertEqual(swo_dap_test.decode_itm_port0(b"\x01H\x01i\x01\n"), "Hi\n")

    def test_handles_all_source_payload_sizes(self):
        self.assertEqual(swo_dap_test.decode_itm_port0(b"\x02Hi\x03ABCD"), "HiABCD")

    def test_skips_other_ports_hardware_and_sync(self):
        packets = b"\x00\x00\x00\x00\x00\x80\x09X\x05Y\x70\x01Z"
        self.assertEqual(swo_dap_test.decode_itm_port0(packets), "Z")

    def test_ignores_truncated_payload(self):
        self.assertEqual(swo_dap_test.decode_itm_port0(b"\x01A\x03B"), "A")

    def test_status_uses_32bit_count(self):
        dev = FakeDevice(b"\x1b\x01\x00\x10\x00\x00")
        self.assertEqual(swo_dap_test.dap_transfer(dev, b"\x1b"), dev.reply)
        self.assertEqual(dev.writes, [(1, b"\x1b" + b"\0" * 511)])

    def test_rejects_truncated_status(self):
        with self.assertRaises(RuntimeError):
            swo_dap_test.dap_transfer(FakeDevice(b"\x1b\x01\0\0\0"), b"\x1b")

    def test_rejects_wrong_echo_and_error(self):
        for reply in (b"\x18\x00", b"\x1a\xff"):
            with self.assertRaises(RuntimeError):
                swo_dap_test.dap_transfer(FakeDevice(reply), b"\x1a\x01")

    def test_rejects_invalid_commands_before_io(self):
        dev = FakeDevice(b"")
        for command in (b"", b"x" * 513):
            with self.assertRaises(ValueError):
                swo_dap_test.dap_transfer(dev, command)
        self.assertEqual(dev.writes, [])

    def test_live_tools_require_explicit_opt_in(self):
        for module in (swo_dap_test, swo_serial_test, swo_target_regs):
            with self.subTest(tool=module.__name__):
                with patch.object(sys, "argv", [module.__name__]), \
                     patch("usb.core.find") as usb_find, \
                     patch("serial.Serial") as serial_open, \
                     redirect_stderr(io.StringIO()), \
                     self.assertRaises(SystemExit) as raised:
                    module.main()
                self.assertEqual(raised.exception.code, 2)
                usb_find.assert_not_called()
                serial_open.assert_not_called()


if __name__ == "__main__":
    unittest.main()
