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
#
# Example (strong check — the kernel prints these milestone banners on boot):
#   SMOKE_SENTINEL='=== Section 7'         make smoke   # reached user space
#   SMOKE_SENTINEL='launching SHELL.ELF'   make smoke   # ring-3 shell launched
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$SCRIPT_DIR"

SMOKE_TIMEOUT="${SMOKE_TIMEOUT:-30}"
SMOKE_SENTINEL="${SMOKE_SENTINEL:-}"
FAIL_PATTERN="${FAIL_PATTERN:-PANIC|Kernel panic|Exception|Unhandled interrupt|Triple fault|General Protection}"

# Build the ISO and user disk (reuses the existing script chain).
. ./config.sh
./iso.sh
./create_disk.sh

LOG=$(mktemp /tmp/quilon-smoke.XXXXXX.log)
trap 'rm -f "$LOG"' EXIT

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
timeout "${SMOKE_TIMEOUT}" "$QEMU" \
    -boot order=d \
    -cdrom quilon.iso \
    -drive file=disk.img,format=raw,if=ide,index=1 \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -smp 2 \
    -display none \
    -no-reboot \
    -serial "file:$LOG"
qrc=$?
set -e

echo "----- serial log (tail) -----"
tail -n 40 "$LOG" || true
echo "-----------------------------"

if grep -Eq "$FAIL_PATTERN" "$LOG"; then
    echo "smoke: FAIL — crash marker found in serial log"
    exit 1
fi

if [ -n "$SMOKE_SENTINEL" ]; then
    if grep -qF "$SMOKE_SENTINEL" "$LOG"; then
        echo "smoke: PASS — sentinel found"
        exit 0
    fi
    echo "smoke: FAIL — sentinel '$SMOKE_SENTINEL' not found"
    exit 1
fi

# No sentinel configured: a clean run (timed out == still alive, or exited 0)
# with no crash markers counts as a pass.
if [ "$qrc" = "124" ] || [ "$qrc" = "0" ]; then
    echo "smoke: PASS — booted ${SMOKE_TIMEOUT}s with no crash markers"
    echo "       (set SMOKE_SENTINEL to a boot-banner string for a stronger check)"
    exit 0
fi

echo "smoke: FAIL — QEMU exited with status $qrc"
exit 1
