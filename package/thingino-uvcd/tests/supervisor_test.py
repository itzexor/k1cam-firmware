#!/usr/bin/env python3
"""Exercise respawn/disconnect/stop without touching a real USB gadget."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time


def until(predicate, timeout=6):
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
    state = root / "gadget"
    # soft_connect stand-in: a FIFO, so every command is seen in order, not
    # just the last one written.
    connection = root / "connection"
    os.mkfifo(connection)
    commands = []

    def record():
        while True:
            with open(connection) as fifo:
                commands.extend(line.strip() for line in fifo)

    threading.Thread(target=record, daemon=True).start()

    def last():
        return commands[-1] if commands else None

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
    source = source.replace("GADGET_STATE=/var/run/uvcd.gadget", f"GADGET_STATE={state}")
    script = root / "supervisor"
    script.write_text(source)
    env = dict(os.environ, SOFT_CONNECT=str(connection))
    proc = subprocess.Popen(["sh", str(script), "supervise"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pidfile.write_text(str(proc.pid))
    child = None
    try:
        until(lambda: commands.count("connect") == 2)
        first = child = int(ready.read_text())
        os.kill(first, signal.SIGKILL)
        until(lambda: last() == "disconnect")
        until(lambda: ready.exists() and ready.read_text() and int(ready.read_text()) != first)
        child = int(ready.read_text())
        until(lambda: commands.count("connect") == 4)
        # Stop the way the init script does: it disconnects, then the
        # supervisor's trap would disconnect again. Neither may reconnect
        # or respawn.
        subprocess.run(["sh", str(script), "stop"], env=env, check=True, timeout=15,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert proc.wait(timeout=3) == 0
        time.sleep(0.2)
        assert last() == "disconnect"
        assert not ready.exists() and not pidfile.exists()
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("supervisor left its daemon alive")
        # Two disconnects in a row make the dwc2 swallow the next connect.
        for a, b in zip(commands, commands[1:]):
            assert not (a == b == "disconnect"), f"double disconnect in {commands}"
        # Every connect is sent twice.
        runs = "".join("c" if c == "connect" else "d" for c in commands)
        assert all(len(r) == 2 for r in runs.split("d") if r), f"connects not doubled: {commands}"
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
