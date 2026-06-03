#!/usr/bin/env python3
"""Headless reproduction harness for the WM startup crash + 'second app hangs'.

Boots the ISO under QEMU with no display, watches the serial log for the shell
prompt, then drives the HMP monitor (over a Unix socket) to type `startx` and
optionally click taskbar buttons. Reports whether the run crashed/hung.

Usage:  tools/repro_startx.py [iterations] [none|click2]
Requires: python3, qemu-system-i386 (no socat/expect needed).
"""
import os, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ITERS = int(sys.argv[1]) if len(sys.argv) > 1 else 1
ACTION = sys.argv[2] if len(sys.argv) > 2 else "none"


def hmp(sock_path, lines):
    """Send HMP command lines to the monitor unix socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    time.sleep(0.2)
    for ln in lines:
        s.sendall((ln + "\n").encode())
        time.sleep(0.05)
    time.sleep(0.2)
    try:
        s.close()
    except OSError:
        pass


def type_startx(sock_path):
    keys = ["s", "t", "a", "r", "t", "x", "ret"]
    hmp(sock_path, [f"sendkey {k}" for k in keys])


def click(sock_path, x, y):
    hmp(sock_path, [f"mouse_move {x} {y}", "mouse_button 1", "mouse_button 0"])


def run_once(n):
    log = tempfile.NamedTemporaryFile(prefix="repro", suffix=".log", delete=False)
    log.close()
    mon = tempfile.mktemp(prefix="repro-mon", suffix=".sock")
    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-boot", "order=d", "-cdrom", os.path.join(ROOT, "quilon.iso"),
        "-drive", f"file={os.path.join(ROOT,'disk.img')},format=raw,if=ide,index=1",
        "-device", "rtl8139,netdev=net0", "-netdev", "user,id=net0",
        "-smp", "2", "-display", "none", "-no-reboot",
        "-serial", f"file:{log.name}",
        "-monitor", f"unix:{mon},server,nowait",
    ])

    def wait_for(token, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if qemu.poll() is not None:
                return False
            try:
                with open(log.name) as f:
                    if token in f.read():
                        return True
            except OSError:
                pass
            time.sleep(0.3)
        return False

    ok = wait_for("quilon>", 30)
    result = {}
    if not ok:
        result["status"] = "no-prompt"
    else:
        # Wait for monitor socket to exist.
        for _ in range(50):
            if os.path.exists(mon):
                break
            time.sleep(0.1)
        type_startx(mon)
        wait_for("[wm] screen", 8)
        time.sleep(3)
        if ACTION == "click2":
            click(mon, 55, 790)   # first taskbar button (Terminal)
            time.sleep(2)
            click(mon, 165, 790)  # second taskbar button (Clock)
            time.sleep(3)

    try:
        hmp(mon, ["quit"])
    except OSError:
        qemu.kill()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()

    text = ""
    try:
        with open(log.name) as f:
            text = f.read()
    finally:
        os.unlink(log.name)

    print(f"=== iteration {n} ===")
    # Anchor on real fault output, not the boot banner that merely prints the
    # SIGSEGV constant. These markers only appear when something actually faults.
    crash_markers = ("--- KERNEL PANIC ---", "-> SIGSEGV",
                     "terminated by signal", "[pf] pid")
    crashed = [l for l in text.splitlines()
               if any(m in l for m in crash_markers) or l.startswith("EIP=")
               or "exited (code -11)" in l]
    started = "[wm] screen" in text
    launches = text.count("[wm] launched")
    print(f"  startx typed: {'yes' if 'startx' in text or started else '?'}; "
          f"WM started: {started}; app launches: {launches}")
    if crashed:
        print("  CRASH:")
        for l in crashed[-8:]:
            print("   ", l.strip())
    elif started and ACTION == "click2" and launches < 2:
        print("  POSSIBLE HANG: WM started but <2 apps launched after 2 clicks")


for i in range(1, ITERS + 1):
    run_once(i)
