#!/usr/bin/env python3
"""qemu_type.py -- drive a headless Quilon VM through the QEMU monitor.

Usage: qemu_type.py <monitor-socket> <serial-log> <wait-for> <script>

Waits until <wait-for> appears in the serial log (i.e. the shell prompt is up),
then plays <script>.  Plain characters are typed with HMP `sendkey`; a literal
"\\n" is Enter.  Commands in braces drive the mouse and pacing:

    {mouse:DX,DY}    relative mouse move (a big negative move pins the cursor
                     to the top-left corner, since the kernel clamps it)
    {click}          left button press + release
    {down} / {up}    left button press / release alone (drag = down, mouse, up)
    {dump:FILE}      QEMU screendump of the display to FILE (PPM)
    {wait:SECONDS}   sleep
    {until:TEXT}     wait (up to 60 s) until TEXT appears in the serial log
    {key:NAME}       one raw HMP sendkey, e.g. {key:ctrl-c}, {key:left},
                     {key:shift-a}, {key:f5}

Used by tools/smoke_test.sh (SMOKE_KEYS), `make ktest` and `make wmtest`.

Uppercase letters and shifted symbols are typed with shift- (the keyboard
driver has handled Shift since ROADMAP3 17.0.1).  Characters that would
collide with the script syntax -- { and } -- need {key:shift-bracket_left} /
{key:shift-bracket_right}.
"""
import re, socket, sys, time

KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '=': 'equal',
        ',': 'comma', ';': 'semicolon', '\n': 'ret', "'": 'apostrophe',
        '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
        '`': 'grave_accent'}

# Shifted characters (US layout) -> the unshifted key to press with shift.
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
           '&': '7', '*': '8', '(': '9', ')': '0', '_': 'minus',
           '+': 'equal', ':': 'semicolon', '"': 'apostrophe', '<': 'comma',
           '>': 'dot', '?': 'slash', '|': 'backslash', '~': 'grave_accent'}


def log_has(log_path, text, start=0):
    try:
        with open(log_path, 'r', errors='replace') as f:
            return text in f.read()[start:]
    except FileNotFoundError:
        return False


def wait_for(log_path, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if log_has(log_path, text):
            return True
        time.sleep(0.25)
    return False


def main():
    sock_path, log_path, prompt, script = sys.argv[1:5]
    script = script.replace('\\n', '\n')

    if not wait_for(log_path, prompt, 120):
        sys.exit("qemu_type: timed out waiting for %r" % prompt)
    time.sleep(1.0)        # let the shell reach its read() loop

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(40):
        try:
            s.connect(sock_path)
            break
        except OSError:
            time.sleep(0.25)

    def hmp(cmd, pause=0.05):
        s.sendall((cmd + '\n').encode())
        time.sleep(pause)

    for tok in re.findall(r'\{[^}]*\}|.', script, re.S):
        if tok.startswith('{') and len(tok) > 1:
            name, _, arg = tok[1:-1].partition(':')
            if name == 'mouse':
                dx, dy = (int(v) for v in arg.split(','))
                # QEMU limits a PS/2 packet to +-255; step large moves.
                while dx or dy:
                    sx = max(-200, min(200, dx))
                    sy = max(-200, min(200, dy))
                    hmp('mouse_move %d %d' % (sx, sy), 0.03)
                    dx -= sx
                    dy -= sy
                time.sleep(0.2)
            elif name == 'click':
                hmp('mouse_button 1', 0.2)
                hmp('mouse_button 0', 0.2)
            elif name == 'down':
                hmp('mouse_button 1', 0.2)
            elif name == 'up':
                hmp('mouse_button 0', 0.2)
            elif name == 'dump':
                hmp('screendump %s' % arg, 0.5)
            elif name == 'wait':
                time.sleep(float(arg))
            elif name == 'key':
                hmp('sendkey %s' % arg)
            elif name == 'until':
                if not wait_for(log_path, arg, 60):
                    sys.exit("qemu_type: timed out waiting for %r" % arg)
            else:
                sys.exit("qemu_type: unknown command %r" % tok)
            continue
        if tok in SHIFTED:
            key = 'shift-' + SHIFTED[tok]
        elif tok.isupper():
            key = 'shift-' + tok.lower()
        else:
            key = KEYS.get(tok, tok)
        if not (len(key) > 1 or tok.isalnum()):
            sys.exit("qemu_type: untypeable character %r" % tok)
        hmp('sendkey %s' % key)
    time.sleep(0.5)
    s.close()


if __name__ == '__main__':
    main()
