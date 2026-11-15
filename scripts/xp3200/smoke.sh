#!/bin/bash
#
# XP3200 smoke test - guards the shared S1C33 CPU core.
#
# Boots the real firmware through the NOR loader, waits for the main menu,
# presses Enter (opens "朗文双解") and Esc (back), exporting LCD frames at
# fixed times through the machine's debug-lcd-dump-* interface.
#
# The firmware images are not part of the repo.  Point the script at them:
#
#   NAND=/path/nand.img NOR=/path/nor.bin QEMU=./_build/qemu-system-s1c33 \
#       bash scripts/xp3200/smoke.sh
#
# Checks:
#   1. the binary registers the xp3200 machine
#   2. the main menu appears and is stable (two frames byte-identical)
#   3. Enter switches to a different screen (the dictionary)
#   4. Esc returns to a menu-like screen
#
# Exit status is 0 only when every check passes.
set -u

QEMU=${QEMU:-./_build/qemu-system-s1c33}
NAND=${NAND:-}
NOR=${NOR:-}
OUT=${OUT:-/tmp/xp3200-smoke}
TAG=${TAG:-xp3200}

FAIL=0
pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); }

[ -x "$QEMU" ] && pass "binary $QEMU" || { fail "binary $QEMU missing"; exit 1; }
[ -n "$NAND" ] && [ -f "$NAND" ] || { fail "NAND image not set (NAND=...)"; exit 1; }
[ -n "$NOR" ] && [ -f "$NOR" ] || { fail "NOR image not set (NOR=...)"; exit 1; }

if "$QEMU" -M help 2>&1 | grep -q '^xp3200 '; then
    pass "machine xp3200 registered"
else
    fail "machine xp3200 not registered"
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.pgm "$OUT"/*.png "$OUT/qmp.sock" "$OUT/qemu.log"
WORK_NAND=$OUT/nand.img
cp "$NAND" "$WORK_NAND"
SOCK=$OUT/qmp.sock
DUMP=$OUT/frame.pgm

"$QEMU" \
    -M "xp3200,nand-image=$WORK_NAND,debug-lcd-dump-path=$DUMP" \
    -bios "$NOR" \
    -cpu c33l05,exit-on-halt=off \
    -display none -serial none -monitor none \
    -qmp unix:$SOCK,server=on,wait=off > "$OUT/qemu.log" 2>&1 &
QPID=$!

TAG="$TAG" python3 - "$SOCK" "$DUMP" <<'PY'
import json, os, socket, sys, time

sock, dump = sys.argv[1], sys.argv[2]
tag = os.environ["TAG"]

qs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(600):
    try:
        qs.connect(sock)
        break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit("qmp: connect failed")
fh = qs.makefile("rwb", buffering=0)
fh.readline()
fh.write(b'{"execute":"qmp_capabilities"}\n')
fh.readline()

def cmd(msg):
    fh.write(json.dumps(msg).encode() + b"\n")
    while True:
        line = fh.readline()
        if not line:
            return {}
        out = json.loads(line.decode())
        if "event" not in out:
            return out

seq = [0]
def shot():
    seq[0] += 1
    cmd({"execute": "qom-set",
         "arguments": {"path": "/machine",
                       "property": "debug-lcd-dump-seq",
                       "value": seq[0]}})
    time.sleep(1.2)

t0 = time.time()
def wait_until(sec):
    while time.time() - t0 < sec:
        time.sleep(0.25)

wait_until(30); shot()
wait_until(45); shot()
cmd({"execute": "send-key",
     "arguments": {"keys": [{"type": "qcode", "data": "ret"}], "hold-time": 120}})
wait_until(55); shot()
cmd({"execute": "send-key",
     "arguments": {"keys": [{"type": "qcode", "data": "esc"}], "hold-time": 120}})
wait_until(65); shot()
cmd({"execute": "quit"})
PY

kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

TAG="$TAG" python3 - "$DUMP" <<'PY'
import glob, os, sys

prefix = sys.argv[1]
frames = sorted(glob.glob(prefix + ".*.pgm"))
if len(frames) != 4:
    print("FAIL  expected 4 frames, got %d" % len(frames))
    sys.exit(1)

def load(path):
    data = open(path, "rb").read()
    head, _, body = data.partition(b"\n255\n")
    magic, dims = head.split(b"\n", 1)
    if magic != b"P5":
        sys.exit("bad PGM %s" % path)
    w, h = (int(x) for x in dims.split())
    return w, h, body

def dark(body):
    return sum(1 for v in body if v < 128)

imgs = [load(p) for p in frames]
w, h = imgs[0][0], imgs[0][1]
menu1, menu2, dict1, back = (imgs[0][2], imgs[1][2], imgs[2][2], imgs[3][2])

ok = True
if (w, h) != (320, 240):
    print("FAIL  frame size %dx%d, expected 320x240" % (w, h)); ok = False
else:
    print("PASS  frame size 320x240")

d_menu = dark(menu1)
if menu1 == menu2:
    print("PASS  main menu is stable (two frames byte-identical)")
else:
    print("FAIL  main menu changed between the two samples"); ok = False

if 12000 <= d_menu <= 16000:
    print("PASS  main menu rendered (%d dark pixels)" % d_menu)
else:
    print("FAIL  main menu dark pixels=%d, expected 12000..16000" % d_menu); ok = False

d_dict = dark(dict1)
if d_dict < d_menu * 0.75:
    print("PASS  Enter opened the dictionary (%d vs %d dark pixels)" % (d_dict, d_menu))
else:
    print("FAIL  Enter did not change the screen (%d vs %d)" % (d_dict, d_menu)); ok = False

d_back = dark(back)
if abs(d_back - d_menu) <= 300:
    print("PASS  Esc returned to the menu (%d dark pixels)" % d_back)
else:
    print("FAIL  Esc did not return to the menu (%d vs %d)" % (d_back, d_menu)); ok = False

sys.exit(0 if ok else 1)
PY
[ $? -eq 0 ] || FAIL=$((FAIL + 1))

echo
if [ "$FAIL" -eq 0 ]; then
    echo "XP3200 SMOKE PASSED (frames in $OUT)"
else
    echo "$FAIL CHECK(S) FAILED (frames in $OUT)"
fi
exit "$FAIL"
