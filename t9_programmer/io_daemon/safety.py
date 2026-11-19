"""Pure request validation; importing this module never accesses hardware."""
from email import policy
from email.parser import BytesParser
from pathlib import Path

MAX_UPLOAD_BYTES = 16 * 1024 * 1024


def upload_bytes(content_type, body):
    if not body or len(body) > MAX_UPLOAD_BYTES:
        raise ValueError("Firmware must contain 1..16 MiB")
    if "multipart/form-data" not in content_type.lower():
        return "firmware.bin", body
    message = BytesParser(policy=policy.default).parsebytes(
        b"MIME-Version: 1.0\r\nContent-Type: " +
        content_type.encode("ascii") + b"\r\n\r\n" + body)
    if not message.is_multipart() or message.defects:
        raise ValueError("Invalid multipart payload")
    files = [part for part in message.iter_parts() if part.get_filename() is not None]
    if len(files) != 1:
        raise ValueError("Exactly one firmware file is required")
    part = files[0]
    filename = Path(part.get_filename().replace("\\", "/")).name
    if filename in ("", ".", ".."):
        raise ValueError("Invalid filename")
    data = part.get_payload(decode=True)
    if not data or len(data) > MAX_UPLOAD_BYTES:
        raise ValueError("Invalid firmware size")
    return filename, data
