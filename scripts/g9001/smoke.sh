#!/bin/bash
#
# SII G9001 smoke test - guards the E08 side of the shared S1C33 CPU core.
#
# The G9001 firmware needs the E08 32-bit address behaviour (trap table and
# the loaded OS live above 0x10000000), so this also verifies that the
# "core-variant" wiring really selects E08 for -M g9001.
#
# The NAND dump is not part of the repo:
#
#   NAND=/path/NAND08GW3B2C.bin QEMU=./_build/qemu-system-s1c33 \
#       bash scripts/g9001/smoke.sh
#
# Checks:
#   1. the binary registers the g9001 machine
#   2. the boot ROM reads its configuration data from NAND
#   3. the firmware OS entry point (0x11e841b0) is executed
#   4. the LCD renderer produces a 640x480 frame with 16 grey levels
#
# The exhaustive 13-check regression lives in the G9001 workspace
# (tools/emu/selftest.sh); this script is the cheap gate that must run
# whenever the shared CPU core changes.
set -u

QEMU=${QEMU:-./_build/qemu-system-s1c33}
NAND=${NAND:-}
OUT=${OUT:-/tmp/g9001-smoke}

FAIL=0
pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); }

[ -x "$QEMU" ] && pass "binary $QEMU" || { fail "binary $QEMU missing"; exit 1; }
[ -n "$NAND" ] && [ -f "$NAND" ] || { fail "NAND image not set (NAND=...)"; exit 1; }

if "$QEMU" -M help 2>&1 | grep -q '^g9001 '; then
    pass "machine g9001 registered"
else
    fail "machine g9001 not registered"
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.pgm

# ---- boot + firmware OS entry ---------------------------------------------
# Run long enough for the loader to relocate and jump into the OS image.
timeout 150 "$QEMU" \
    -M "g9001,nand-image=$NAND" \
    -cpu c33l05,exit-on-halt=off,trace-exec=on,trace-exec-start=0x11e80000,trace-exec-end=0x11ea0000 \
    -display none -monitor none -serial none \
    -d guest_errors -D "$OUT/boot.log" > "$OUT/boot.out" 2>&1

if grep -q 'boot config data' "$OUT/boot.out"; then
    pass "boot ROM read its configuration data from NAND"
else
    fail "boot configuration data not read"
fi

if grep -q 's1c33-exec: tb pc=0x11e841b0' "$OUT/boot.log"; then
    pass "firmware OS entry executed (E08 32-bit jump target)"
else
    fail "firmware OS entry never executed - is core-variant=E08 active?"
fi

# ---- LCD renderer ----------------------------------------------------------
timeout 8 "$QEMU" \
    -M "g9001,nand-image=$NAND,debug-lcd-test=on,debug-lcd-dump-ms=100,debug-lcd-dump-path=$OUT/lcd.pgm" \
    -cpu c33l05,exit-on-halt=off \
    -display none -monitor none -serial none > "$OUT/lcd.out" 2>&1

python3 - "$OUT/lcd.pgm" "$OUT/lcd.out" <<'PY'
import sys

path, log = sys.argv[1], sys.argv[2]
text = open(log, "rb").read().decode("utf-8", "replace")
if "dumped 640x480 4bpp" not in text:
    print("FAIL  LCD frame not exported")
    sys.exit(1)

data = open(path, "rb").read()
head, _, body = data.partition(b"\n255\n")
magic, dims = head.split(b"\n", 1)
w, h = (int(x) for x in dims.split())
levels = len(set(body))
if (w, h) == (640, 480) and levels == 16:
    print("PASS  LCD frame 640x480 4bpp with 16 grey levels")
    sys.exit(0)
print("FAIL  LCD frame %dx%d with %d grey levels" % (w, h, levels))
sys.exit(1)
PY
[ $? -eq 0 ] || FAIL=$((FAIL + 1))

echo
if [ "$FAIL" -eq 0 ]; then
    echo "G9001 SMOKE PASSED (logs in $OUT)"
else
    echo "$FAIL CHECK(S) FAILED (logs in $OUT)"
fi
exit "$FAIL"
