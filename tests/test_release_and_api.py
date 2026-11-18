"""Offline artifact/auth/upload validation; never imports the live daemon."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "t9_programmer" / "io_daemon"))
from bit_to_pcap import bit_to_pcap
from release_manifest import package, verify, source_hashes
from safety import authorized, upload_bytes


def test_bit(payload=b"\xaa\x99\x55\x66"):
    prefix = struct.pack(">H", 9) + b"\x0f\xf0" * 4 + b"\0" + b"\0\x01a"
    for tag in (b"name\0", b"part\0", b"date\0", b"time\0"):
        prefix += struct.pack(">H", len(tag)) + tag
        prefix += (b"b", b"c", b"d", b"e")[(b"name\0", b"part\0", b"date\0", b"time\0").index(tag)]
    return prefix + struct.pack(">I", len(payload)) + payload


class ReleaseAndApiTests(unittest.TestCase):
    def test_converter_and_truncation(self):
        data = test_bit()
        self.assertEqual(bit_to_pcap(data), b"\x66\x55\x99\xaa")
        for end in range(len(data)):
            with self.assertRaises((ValueError, struct.error)):
                bit_to_pcap(data[:end])
        with self.assertRaises(ValueError):
            bit_to_pcap(data + b"x")

    def test_upload_preserves_binary_trailing_crlf(self):
        firmware = b"\x00\xff\x01\r\n\r\n"
        body = (b"--test\r\nContent-Disposition: form-data; name=\"file\"; "
                b"filename=\"image.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
                firmware + b"\r\n--test--\r\n")
        name, data = upload_bytes("multipart/form-data; boundary=test", body)
        self.assertEqual((name, data), ("image.bin", firmware))
        self.assertEqual(hashlib.sha256(data).digest(), hashlib.sha256(firmware).digest())

    def test_filename_is_basename_and_raw_upload_unchanged(self):
        data = b"\xff\r\n"
        self.assertEqual(upload_bytes("application/octet-stream", data)[1], data)
        with self.assertRaises(ValueError):
            upload_bytes("multipart/form-data; boundary=x", b"not multipart")

    def test_auth_fails_closed(self):
        token = "a" * 64
        self.assertTrue(authorized("Bearer " + token, token))
        self.assertFalse(authorized("Bearer \N{LATIN SMALL LETTER E WITH ACUTE}", token))
        for header, configured in (("", ""), ("Bearer a", "a"),
                                   ("Bearer " + "b" * 64, token), (None, token)):
            self.assertFalse(authorized(header, configured))

    def test_release_rejects_stale_artifacts_and_sources(self):
        base = Path(tempfile.gettempdir()) / "opencode"
        base.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            source = root / "scripts" / "example.py"
            source.write_text("# source\n")
            snapshot = root / "snapshot.json"
            snapshot.write_text(json.dumps(source_hashes(root)))
            bit = root / "test.bit"
            bit.write_bytes(test_bit())
            hwh = root / "test.hwh"
            hwh.write_bytes(b"axi_swd_0 axi_swd_1 axi_swo_0 43C20000")
            output = root / "release"
            package(root, bit, hwh, snapshot, output)
            verify(output, root)
            source.write_bytes(b"# source\r\n")
            verify(output, root)  # Windows/Linux text checkout normalization.
            source.write_text("# changed\n")
            with self.assertRaises(ValueError):
                verify(output, root)
            source.write_text("# source\n")
            (output / "t9_programmer_pcap.bin").write_bytes(b"\0" * 4)
            with self.assertRaises(ValueError):
                verify(output, root)


if __name__ == "__main__":
    unittest.main()
