"""Cross-process leases shared with the C probe server."""
import os


class ResourceBusy(RuntimeError):
    pass


class ResourceLease:
    def __init__(self, *resources):
        self.fds = []
        try:
            import fcntl
            for resource in sorted(resources):
                fd = os.open(f"/run/lock/plink-{resource}.lock",
                             os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o660)
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    os.close(fd)
                    raise ResourceBusy(f"{resource} is owned by another operation")
                self.fds.append(fd)
        except Exception:
            self.close()
            raise

    def close(self):
        for fd in self.fds:
            os.close(fd)
        self.fds.clear()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()
