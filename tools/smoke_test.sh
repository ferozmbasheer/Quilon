#!/bin/sh
# smoke_test.sh — headless QEMU boot smoke test.
#
# Boots the freshly built ISO under QEMU with no graphical display, captures the
# serial console to a log, and decides pass/fail without human interaction. This
# is the automatable version of `./qemu.sh` and is intended for use from
# `make smoke` (and, later, CI).
#
# Pass/fail logic:
#   * FAIL  if the serial log contains a panic / CPU-exception marker
#           (see FAIL_PATTERN below), OR if QEMU itself errors out.
#   * If SMOKE_SENTINEL is set, additionally REQUIRE that string to appear in
#     the log (strong check — proves boot reached a known-good point).
#   * If SMOKE_SENTINEL is empty, PASS as long as QEMU ran for the full timeout
#     without hitting a FAIL_PATTERN (weak check — proves it didn't crash).
#
# Tunables (environment variables):
#   SMOKE_TIMEOUT   seconds to let the VM run            (default 30)
#   SMOKE_SENTINEL  success string to require in the log (default empty)
#   FAIL_PATTERN    egrep pattern marking a crash        (default below)
#   SMOKE_DISK      disk image to attach                 (default disk.img)
#                   e.g. SMOKE_DISK=ext2.img to boot the ext2 image built by
#                   `make -C user ext2disk` (section 16.2)
#   SMOKE_KEYS      text to type into the shell once its prompt is up, via the
#                   QEMU monitor (tools/qemu_type.py); "\n" is Enter.
#                   e.g. SMOKE_KEYS='exec ktest.elf\n' SMOKE_SENTINEL='KTEST: ALL PASS'
#                   is what `make ktest` runs.  Braces add mouse/pacing
#                   commands -- see tools/qemu_type.py.
#   SMOKE_SENTINEL_COUNT  require the sentinel at least this many times
#                   (default 1); `make wmtest` uses it to count shell exits.
#
# Example (strong check — the kernel prints these milestone banners on boot):
#   SMOKE_SENTINEL='=== Section 7'         make smoke   # reached user space
#   SMOKE_SENTINEL='launching SHELL.ELF'   make smoke   # ring-3 shell launched
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$SCRIPT_DIR"

SMOKE_TIMEOUT="${SMOKE_TIMEOUT:-30}"
# Default: the ring-3 shell's banner, so a boot that hangs before user space
# fails instead of "passing" by not crashing.  SMOKE_SENTINEL='' disables it.
SMOKE_SENTINEL="${SMOKE_SENTINEL-Type 'help' for a list of commands.}"
SMOKE_DISK="${SMOKE_DISK:-disk.img}"
SMOKE_KEYS="${SMOKE_KEYS:-}"
SMOKE_SENTINEL_COUNT="${SMOKE_SENTINEL_COUNT:-1}"
SMOKE_PROMPT="${SMOKE_PROMPT:-Type 'help' for a list of commands.}"
FAIL_PATTERN="${FAIL_PATTERN:-PANIC|Kernel panic|Exception|Unhandled interrupt|Triple fault|General Protection}"

# Build the ISO and user disk (reuses the existing script chain).
. ./config.sh
./iso.sh
./create_disk.sh

LOG=$(mktemp /tmp/quilon-smoke.XXXXXX.log)
MON=$(mktemp -u /tmp/quilon-smoke.XXXXXX.mon)
FAIL_LOG="${SMOKE_FAIL_LOG:-$SCRIPT_DIR/smoke-fail.log}"
trap 'rm -f "$LOG" "$MON"' EXIT
# Keep the full serial log of a failing run for diagnosis (gitignored).
fail() { cp "$LOG" "$FAIL_LOG" 2>/dev/null; echo "$1"; echo "smoke: full serial log kept in $FAIL_LOG"; exit 1; }

QEMU="qemu-system-$(./target-triplet-to-arch.sh "$HOST")"
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "smoke: $QEMU not found on PATH"; exit 2; }
command -v timeout >/dev/null 2>&1 || {
    echo "smoke: 'timeout' (coreutils) not found on PATH"; exit 2; }

echo "smoke: booting (timeout ${SMOKE_TIMEOUT}s, sentinel='${SMOKE_SENTINEL:-<none>}')..."

# -display none + -serial file: fully headless. -no-reboot so a triple fault
# stops the VM instead of looping. timeout bounds the run; 124 == timed out,
# which is the EXPECTED exit for a VM that boots and idles at a prompt.
set +e
TYPER_PID=
if [ -n "$SMOKE_KEYS" ]; then
    python3 tools/qemu_type.py "$MON" "$LOG" "$SMOKE_PROMPT" "$SMOKE_KEYS" &
    TYPER_PID=$!
fi
timeout "${SMOKE_TIMEOUT}" "$QEMU" \
    -monitor "unix:$MON,server,nowait" \
    -boot order=d \
    -cdrom quilon.iso \
    -drive file="$SMOKE_DISK",format=raw,if=ide,index=1 \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -smp 2 \
    -display none \
    -no-reboot \
    -serial "file:$LOG" &
QEMU_PID=$!

# Stop early once the outcome is known -- the sentinel has appeared often
# enough, or a crash marker has -- instead of always sitting out the timeout.
# Without a sentinel the run must last the full timeout (that IS the test).
# Give a just-matched sentinel a moment so trailing output lands in the log.
while kill -0 "$QEMU_PID" 2>/dev/null; do
    sleep 1
    if grep -Eq "$FAIL_PATTERN" "$LOG" 2>/dev/null; then
        sleep 1; kill "$QEMU_PID" 2>/dev/null; break
    fi
    if [ -n "$SMOKE_SENTINEL" ] &&
       [ "$(grep -cF "$SMOKE_SENTINEL" "$LOG" 2>/dev/null || true)" -ge "$SMOKE_SENTINEL_COUNT" ]; then
        sleep 1; kill "$QEMU_PID" 2>/dev/null; break
    fi
done
wait "$QEMU_PID"
qrc=$?
[ -n "$TYPER_PID" ] && { kill "$TYPER_PID" 2>/dev/null; wait "$TYPER_PID" 2>/dev/null; }
set -e

echo "----- serial log (tail) -----"
tail -n 40 "$LOG" || true
echo "-----------------------------"

if grep -Eq "$FAIL_PATTERN" "$LOG"; then
    fail "smoke: FAIL — crash marker found in serial log"
fi

if [ -n "$SMOKE_SENTINEL" ]; then
    found=$(grep -cF "$SMOKE_SENTINEL" "$LOG" || true)
    if [ "$found" -ge "$SMOKE_SENTINEL_COUNT" ]; then
        echo "smoke: PASS — sentinel found ($found/$SMOKE_SENTINEL_COUNT)"
        exit 0
    fi
    fail "smoke: FAIL — sentinel '$SMOKE_SENTINEL' found $found/$SMOKE_SENTINEL_COUNT times"
fi

# No sentinel configured: a clean run (timed out == still alive, or exited 0)
# with no crash markers counts as a pass.
if [ "$qrc" = "124" ] || [ "$qrc" = "0" ]; then
    echo "smoke: PASS — booted ${SMOKE_TIMEOUT}s with no crash markers"
    echo "       (set SMOKE_SENTINEL to a boot-banner string for a stronger check)"
    exit 0
fi

fail "smoke: FAIL — QEMU exited with status $qrc"
