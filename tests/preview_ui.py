"""Offline UI preview: localhost, fake pin state, no hardware initialization."""
from unittest.mock import patch
from test_web_api import daemon, mock_server

if __name__ == "__main__":
    class PreviewHandler(daemon.RestApiHandler):
        def do_POST(self):
            self.send_json({"error": "Preview only; no hardware is connected"}, 403)
            self.close_connection = True
    server = mock_server(PreviewHandler)
    print(f"Mock Plink UI: http://127.0.0.1:{server.server_port}", flush=True)
    print("Read-only preview; all POST requests are rejected.", flush=True)
    with patch.object(daemon.SysfsGPIO, "read", return_value=None):
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
