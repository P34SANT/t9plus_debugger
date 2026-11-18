"""Offline UI preview: localhost, fake pin state, no hardware initialization."""
from unittest.mock import patch
from test_web_api import daemon, mock_server

if __name__ == "__main__":
    server = mock_server()
    print(f"Mock Plink UI: http://127.0.0.1:{server.server_port}", flush=True)
    print("Read-only preview; all POST requests are rejected.", flush=True)
    daemon.API_TOKEN = ""
    with patch.object(daemon.SysfsGPIO, "read", return_value=None):
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
