"""Local HTTP tests with fake GPIO/managers; never calls run_server()."""
from contextlib import nullcontext
import http.client
import io
import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "t9_programmer" / "io_daemon"))
import io_daemon as daemon


def mock_server():
    daemon.API_TOKEN = "t" * 64
    daemon.d3_manager = SimpleNamespace(mode="idle")
    pins = json.loads((ROOT / "t9_programmer/io_daemon/pins.json").read_text())
    daemon.pin_manager = SimpleNamespace(
        pins=pins, resolve_pin=lambda query: (query, pins.get(query)))
    return daemon.ThreadedHTTPServer(("127.0.0.1", 0), daemon.RestApiHandler)


class WebApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = mock_server()
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def request(self, path, body=None, headers=None, method="GET"):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=3)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_assets_and_security_headers(self):
        for path, content_type in (("/", "text/html"), ("/app.css", "text/css"),
                                   ("/app.js", "text/javascript")):
            status, headers, body = self.request(path)
            self.assertEqual(status, 200)
            self.assertTrue(headers["Content-Type"].startswith(content_type))
            self.assertEqual(len(body), int(headers["Content-Length"]))
            self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
        self.assertEqual(self.request("/web/../pins.json")[0], 404)
        self.assertEqual(self.request("/pins.json")[0], 404)

    def test_canonical_pinmap(self):
        status, _, body = self.request("/api/pinmap")
        self.assertEqual(status, 200)
        mapping = json.loads(body)
        self.assertEqual(next(p["fpga"] for p in mapping["pins"]
                              if p["header"] == "J3" and p["pin"] == 6), "T10")

    def test_auth_and_json_validation(self):
        self.assertEqual(self.request("/api/unknown", "{}", method="POST")[0], 401)
        headers = {"Authorization": "Bearer " + daemon.API_TOKEN}
        self.assertEqual(self.request("/api/unknown", "{broken", headers, "POST")[0], 400)
        self.assertEqual(self.request("/api/unknown", "[]", headers, "POST")[0], 400)
        self.assertEqual(self.request("/api/unknown", b"\xff", headers, "POST")[0], 400)
        self.assertEqual(self.request("/api/unknown", "{}", {
            **headers, "Origin": "http://attacker.invalid"}, "POST")[0], 403)
        with patch.object(daemon, "ResourceLease", return_value=nullcontext()):
            self.assertEqual(self.request("/api/serial/send",
                '{"channel":2,"text":"x"}', headers, "POST")[0], 400)

    def test_unknown_gpio_is_not_low(self):
        with patch.object(daemon.os.path, "exists", return_value=False), \
                patch.object(daemon.SysfsGPIO, "export"):
            self.assertIsNone(daemon.SysfsGPIO.read(99999))
        with patch.object(daemon.SysfsGPIO, "read", return_value=None):
            status, _, body = self.request("/api/pins")
        self.assertEqual(status, 200)
        self.assertIsNone(json.loads(body)["boot0"]["current_val"])

    def test_sse_preserves_multiline_and_spaces(self):
        handler = object.__new__(daemon.RestApiHandler)
        handler.wfile = io.BytesIO()
        text = "  first\nsecond\r\n "
        handler.send_event({"text": text})
        encoded = handler.wfile.getvalue().decode()
        self.assertEqual(encoded.count("\n"), 2)
        self.assertEqual(json.loads(encoded.removeprefix("data: "))["text"], text)

    def test_reset_releases_even_if_sleep_fails(self):
        manager = object.__new__(daemon.PinManager)
        manager.pins = daemon.pin_manager.pins
        with patch.object(daemon.SysfsGPIO, "set_direction") as direction, \
                patch.object(daemon.time, "sleep", side_effect=RuntimeError("interrupted")):
            with self.assertRaises(RuntimeError):
                manager.pulse("target_nrst")
            self.assertEqual([call.args[1] for call in direction.call_args_list], ["low", "in"])

    def test_source_contains_no_storage_or_external_scripts(self):
        source = (ROOT / "t9_programmer/io_daemon/web/app.js").read_text(encoding="utf-8")
        self.assertNotIn("localStorage", source)
        self.assertNotIn("sessionStorage", source)
        self.assertNotIn("token=", source)

    def test_flashing_subprocess_is_bounded_and_closed(self):
        from unittest.mock import Mock
        process = Mock()
        process.stdout = io.StringIO("progress\n")
        process.wait.return_value = 0
        process.poll.return_value = 0
        timer = Mock()
        with patch.object(daemon.subprocess, "Popen", return_value=process) as popen, \
                patch.object(daemon.threading, "Timer", return_value=timer):
            self.assertEqual(daemon.run_flash_command(["fake-tool"]), 0)
        self.assertTrue(popen.call_args.kwargs["start_new_session"])
        timer.start.assert_called_once()
        timer.cancel.assert_called_once()
        self.assertTrue(process.stdout.closed)


if __name__ == "__main__":
    unittest.main()
