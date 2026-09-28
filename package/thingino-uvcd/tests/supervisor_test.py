#!/usr/bin/env python3
"""Exercise respawn/disconnect/stop without touching a real USB gadget."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


def until(predicate, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("timed out waiting for supervisor")


with tempfile.TemporaryDirectory(prefix="uvcd-supervisor-") as tmp:
    root = Path(tmp)
    ready = root / "ready"
    pidfile = root / "supervisor.pid"
    connection = root / "connection"
    connection.write_text("disconnect\n")
    daemon = root / "daemon"
    daemon.write_text(
        "#!/usr/bin/env python3\n"
        "import os, time\n"
        f"open({str(ready)!r}, 'w').write(str(os.getpid()))\n"
        "while True: time.sleep(0.1)\n"
    )
    daemon.chmod(0o755)
    source = (Path(__file__).parent / "../files/S31uvcd").read_text()
    source = source.replace("DAEMON=/usr/bin/uvcd", f"DAEMON={daemon}")
    source = source.replace("SUPERVISOR_PID=/var/run/uvcd-supervisor.pid", f"SUPERVISOR_PID={pidfile}")
    source = source.replace("READY=/var/run/uvcd.ready", f"READY={ready}")
    script = root / "supervisor"
    script.write_text(source)
    env = dict(os.environ, SOFT_CONNECT=str(connection))
    proc = subprocess.Popen(["sh", str(script), "supervise"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pidfile.write_text(str(proc.pid))
    child = None
    try:
        until(lambda: connection.read_text().strip() == "connect")
        first = child = int(ready.read_text())
        os.kill(first, signal.SIGKILL)
        until(lambda: connection.read_text().strip() == "disconnect")
        until(lambda: ready.exists() and ready.read_text() and int(ready.read_text()) != first)
        child = int(ready.read_text())
        until(lambda: connection.read_text().strip() == "connect")
        # Stop while waiting for the daemon; it must neither reconnect nor respawn.
        proc.terminate()
        assert proc.wait(timeout=3) == 0
        assert connection.read_text().strip() == "disconnect"
        assert not ready.exists() and not pidfile.exists()
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("supervisor left its daemon alive")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if child:
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
print("supervisor regression tests passed")
