"""Pure native-driver planning/package checks; importing never probes hardware."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("plink_native_activate",
    ROOT / "linux/uartlite/activate.py")
activate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(activate)


class NativeUartTests(unittest.TestCase):
    def qualifier(self):
        spec = importlib.util.spec_from_file_location("plink_native_qualify",
            ROOT / "linux/uartlite/qualify_serial.py")
        qualify = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(qualify)
        return qualify

    def make_tree(self, root):
        bus = root / "axi"
        clock = bus / "slcr@f8000000/clkc@100"
        irq = bus / "interrupt-controller@f8f01000"
        clock.mkdir(parents=True)
        irq.mkdir(parents=True)
        for folder, name, value in ((bus, "#address-cells", 1), (bus, "#size-cells", 1),
                                    (clock, "#clock-cells", 1), (clock, "phandle", 11),
                                    (irq, "#interrupt-cells", 3), (irq, "phandle", 22)):
            (folder / name).write_bytes(struct.pack(">I", value))
        (clock / "compatible").write_bytes(b"xlnx,ps7-clkc\0")
        (irq / "compatible").write_bytes(b"arm,cortex-a9-gic\0")

    def test_overlay_uses_live_handles_and_rising_irq(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_tree(root)
            source = activate.overlay_source(root)
            self.assertIn("clocks = <&plink_uartlite_clock>", source)
            self.assertIn("clock-frequency = <100000000>", source)
            self.assertIn("interrupt-parent = <0x16>", source)
            self.assertIn("interrupts = <0 29 1>", source)
            self.assertIn('clock-names = "s_axi_aclk"', source)
            self.assertIn("0x42c00000 0x10000", source)

    def test_overlay_rejects_unknown_bus_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_tree(root)
            (root / "axi/#address-cells").write_bytes(struct.pack(">I", 2))
            with self.assertRaises(ValueError):
                activate.overlay_source(root)

    def test_package_tamper_and_wrong_kernel_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ("uartlite.ko", "activate.py", "native_bridge.py",
                     "plink-uartlite.service", "native-bridge.conf")
            for name in names:
                (root / name).write_bytes(b"test")
            manifest = {"kernel_release": activate.EXPECTED_RELEASE,
                        "kernel_commit": activate.KERNEL_COMMIT,
                        "files": {name: hashlib.sha256(b"test").hexdigest() for name in names}}
            (root / "manifest.json").write_text(json.dumps(manifest))
            activate.verify_package(root)
            (root / "uartlite.ko").write_bytes(b"modified")
            with self.assertRaises(ValueError):
                activate.verify_package(root)
            manifest["kernel_release"] = "5.15-unrelated"
            (root / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                activate.verify_package(root)

    def test_unsafe_package_filename_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ("uartlite.ko", "activate.py", "native_bridge.py",
                     "plink-uartlite.service", "native-bridge.conf")
            files = {name: hashlib.sha256(b"test").hexdigest() for name in names}
            for name in names:
                (root / name).write_bytes(b"test")
            files["../outside"] = "not-a-digest"
            manifest = {"kernel_release": activate.EXPECTED_RELEASE,
                        "kernel_commit": activate.KERNEL_COMMIT, "files": files}
            (root / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                activate.verify_package(root)

    def test_normal_module_loading_never_uses_force(self):
        source = (ROOT / "linux/uartlite/activate.py").read_text(encoding="utf-8")
        self.assertIn('run("insmod", str(PACKAGE / "uartlite.ko"))', source)
        self.assertNotIn("--force", source)
        self.assertNotIn("rmmod -f", source)

    def test_kernel_counter_layout_and_native_backend(self):
        native_spec = importlib.util.spec_from_file_location("plink_native_bridge",
            ROOT / "linux/uartlite/native_bridge.py")
        native = importlib.util.module_from_spec(native_spec)
        with patch.dict(sys.modules, {"activate": activate}):
            native_spec.loader.exec_module(native)
        class Base:
            def __init__(self):
                self.fd = 123
                self.port_index = 1
                self.stats = {}
            def _open_tty_locked(self):
                return True
        def ioctl(fd, operation, buffer, mutate):
            self.assertEqual(operation, 0x545D)
            self.assertEqual(len(buffer), 80)
            buffer[:] = struct.pack("=20i", *range(20))
        fake = SimpleNamespace(TTYBridgeServer=Base, fcntl=SimpleNamespace(ioctl=ioctl))
        tty = native.native_class(fake)()
        self.assertTrue(tty._open_tty_locked())
        self.assertEqual(tty.stats["backend"], "kernel_tty_irq")
        self.assertEqual(tty.stats["kernel_rx"], 4)
        self.assertEqual(tty.stats["hardware_overruns"], 7)
        self.assertEqual(tty.stats["kernel_buffer_overruns"], 10)
        self.assertEqual(tty.stats["counter_state"], "measured")

    def test_serial_analysis_preserves_complete_boundary_lines(self):
        qualify = self.qualifier()
        line1 = b"[USART1] STM32 Pulse #1 | Tick: 500 ms\r\n"
        line2 = b"[USART1] STM32 Pulse #2 | Tick: 1000 ms\r\n"
        self.assertEqual(qualify.analyze(line1 + line2)["ids"], [1, 2])
        self.assertEqual(qualify.analyze(b"partial\r\n" + line1 + b"tail")["ids"], [1])
        self.assertEqual(qualify.analyze(line1 + b"BAD\r\n" + line2)["malformed_lines"], 1)
        self.assertEqual(qualify.analyze(line1 + line2)["ticks"], [500, 1000])

    def test_qualification_rejects_unknown_stale_and_error_counters(self):
        qualify = self.qualifier()
        uart = {"backend": "kernel_tty_irq", "counter_state": "measured",
                "hardware_overruns": 0, "kernel_buffer_overruns": 0,
                "framing_errors": 0, "parity_errors": 0}
        snapshot = {"uart": {"age_seconds": 0.1, "metrics": {"uart1": uart}},
                    "pod1": {"age_seconds": 0.1, "metrics": {"swo": {
                        "software_dropped": 0, "hardware_dropped": 0, "framing_errors": 0}}}}
        self.assertFalse(qualify.error_deltas(snapshot, snapshot, True)[1])
        self.assertTrue(qualify.error_deltas({}, snapshot, True)[1])
        changed = json.loads(json.dumps(snapshot))
        changed["uart"]["age_seconds"] = 30
        self.assertTrue(qualify.error_deltas(snapshot, changed, True)[1])
        changed = json.loads(json.dumps(snapshot))
        changed["pod1"]["metrics"]["swo"]["hardware_dropped"] = 1
        self.assertTrue(qualify.error_deltas(snapshot, changed, True)[1])
        changed = json.loads(json.dumps(snapshot))
        changed["uart"]["metrics"]["uart1"]["hardware_overruns"] = 1
        deltas, errors = qualify.error_deltas(snapshot, changed, True)
        self.assertEqual(deltas["hardware_overruns"], 1)
        self.assertTrue(errors)


if __name__ == "__main__":
    unittest.main()
