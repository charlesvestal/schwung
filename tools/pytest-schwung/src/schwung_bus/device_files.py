"""SSH-backed file operations on the Move device.

Used by the ``pristine_set`` fixture to copy a template Song.abl
file onto Move's filesystem and trigger a restart. Keeps an
SSH ControlMaster connection open for the lifetime of the test
session so the per-test cost is one quick command over the
existing socket instead of a fresh SSH handshake (~200 ms saved
per call).

Scope: deliberately tiny. ``put_file``, ``run`` (for `cp`, `ls`,
etc.), and lifecycle management. Anything richer (jq-style JSON
patching, recursive sync) should live in a separate module.

Why not paramiko: avoiding a third-party dep keeps the test harness
installable as a plain `pip install -e .` without C-build pain.
The OpenSSH CLI is universally available on dev machines that talk
to Move and supports ControlMaster natively.
"""

from __future__ import annotations

import hashlib
import os
import shlex
import subprocess
from pathlib import Path
from typing import Optional


DEFAULT_HOST = "ableton@move.local"
# Short path required: SSH adds a suffix to ControlPath and the full
# sockaddr_un is capped at ~104 chars on macOS / 108 on Linux. Living
# under ~/.ssh keeps it bounded and avoids the user's $TMPDIR which
# on macOS is /var/folders/.../T/ (already ~50 chars).
DEFAULT_CONTROL_DIR = Path.home() / ".ssh" / "cm"


class DeviceFilesError(RuntimeError):
    """SSH/scp returned non-zero or output didn't parse as expected."""


class DeviceFiles:
    """Persistent SSH connection for file ops + remote commands on Move.

    Usage::

        with DeviceFiles() as dev:
            dev.put_file("local.abl", "/data/UserData/staging.abl")
            dev.run("cp /data/UserData/staging.abl /target/path/Song.abl")

    On enter, opens an SSH ControlMaster socket. On exit, closes it.
    Subsequent ssh/scp invocations from the same instance reuse the
    socket — no handshake. ~50 ms per call instead of ~200 ms.

    Not thread-safe. The fixture is session-scoped and single-threaded.
    """

    def __init__(
        self,
        host: Optional[str] = None,
        control_dir: Optional[Path] = None,
        connect_timeout: float = 10.0,
    ) -> None:
        self.host = host or os.environ.get("SCHWUNG_DEVICE_SSH", DEFAULT_HOST)
        self.control_dir = control_dir or DEFAULT_CONTROL_DIR
        self.connect_timeout = connect_timeout
        # Hash the host into 8 hex chars — keeps the full sockaddr_un
        # path well under the 104-char macOS / 108-char Linux limit
        # (OpenSSH appends ~17 chars of suffix). Multiple Move devices
        # on the same dev machine get distinct sockets via the hash.
        host_hash = hashlib.sha1(self.host.encode()).hexdigest()[:8]
        self.control_path = self.control_dir / host_hash
        self._opened = False

    # ----- lifecycle ---------------------------------------------------------

    def open(self) -> None:
        """Open the persistent ControlMaster connection.

        Idempotent — if a master is already running for this control
        path, OpenSSH reuses it. We always run a no-op `true` over
        the connection on first open so a broken socket fails loudly
        here rather than on the first real call.
        """
        if self._opened:
            return
        self.control_dir.mkdir(parents=True, exist_ok=True)
        # Probe + auto-spawn master via -M -f -N pattern. Then send a
        # no-op `true` over it to confirm the master actually answers.
        # We don't `ssh -M -f -N` separately because OpenSSH's
        # ControlMaster=auto with ControlPersist=N already spawns the
        # master on first use of any ssh that references this path.
        self._opened = True
        try:
            self.run("true")
        except Exception:
            self._opened = False
            raise

    def close(self) -> None:
        """Tear down the ControlMaster connection."""
        if not self._opened:
            return
        try:
            subprocess.run(
                self._ssh_base() + ["-O", "exit", self.host],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except Exception:
            # Cleanup must never raise — the test session is ending,
            # we don't want a teardown error to mask a real test failure.
            pass
        finally:
            self._opened = False

    def __enter__(self) -> "DeviceFiles":
        self.open()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ----- operations --------------------------------------------------------

    def run(self, command: str, check: bool = True) -> subprocess.CompletedProcess:
        """Run a shell command on the device. Returns the completed process.

        Raises ``DeviceFilesError`` on non-zero exit when ``check`` is true
        (default). Stdout/stderr are captured; access via ``.stdout`` /
        ``.stderr`` on the return value.
        """
        argv = self._ssh_base() + [self.host, command]
        result = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=self.connect_timeout,
        )
        if check and result.returncode != 0:
            raise DeviceFilesError(
                f"remote command failed (exit {result.returncode}): "
                f"{command!r}\nstdout: {result.stdout!r}\nstderr: {result.stderr!r}"
            )
        return result

    def put_file(self, local_path: str | Path, remote_path: str) -> None:
        """Copy a local file to ``remote_path`` on the device.

        Uses ``scp`` over the same ControlMaster socket. Remote
        directory must already exist — scp's `-r` and auto-mkdir are
        intentionally NOT used; the caller is responsible for path
        correctness so a typo doesn't silently create rubbish dirs
        on the user's device.
        """
        local = Path(local_path)
        if not local.is_file():
            raise DeviceFilesError(f"local file does not exist: {local_path}")
        argv = self._scp_base() + [str(local), f"{self.host}:{remote_path}"]
        result = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=self.connect_timeout,
        )
        if result.returncode != 0:
            raise DeviceFilesError(
                f"scp {local} → {self.host}:{remote_path} failed: "
                f"{result.stderr.strip()}"
            )

    def read_text(self, remote_path: str) -> str:
        """Read a remote text file as a string."""
        result = self.run(f"cat {shlex.quote(remote_path)}")
        return result.stdout

    def file_exists(self, remote_path: str) -> bool:
        """True if remote path exists (file or directory)."""
        result = self.run(f"test -e {shlex.quote(remote_path)}", check=False)
        return result.returncode == 0

    # ----- internals ---------------------------------------------------------

    def _ssh_base(self) -> list[str]:
        """ssh arg prefix with ControlMaster config."""
        return [
            "ssh",
            "-o", "ControlMaster=auto",
            "-o", f"ControlPath={self.control_path}",
            # Persist 1 hour after last use — covers slow test sessions
            # without leaking forever if pytest crashes hard.
            "-o", "ControlPersist=3600",
            "-o", f"ConnectTimeout={int(self.connect_timeout)}",
            "-o", "ServerAliveInterval=30",
            # Don't prompt for unknown hosts during automated runs;
            # the test harness's first SSH already added the key.
            "-o", "StrictHostKeyChecking=accept-new",
        ]

    def _scp_base(self) -> list[str]:
        """scp arg prefix — reuses the same ControlMaster socket."""
        return [
            "scp",
            "-o", "ControlMaster=auto",
            "-o", f"ControlPath={self.control_path}",
            "-o", "ControlPersist=3600",
            "-o", f"ConnectTimeout={int(self.connect_timeout)}",
            "-o", "ServerAliveInterval=30",
            "-o", "StrictHostKeyChecking=accept-new",
        ]
