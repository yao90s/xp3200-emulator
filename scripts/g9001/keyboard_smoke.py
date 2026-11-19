#!/usr/bin/env python3
"""Exercise the G9001 firmware keyboard through QMP and the rendered LCD."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
QEMU = ROOT / "_build/qemu-system-s1c33"
BAD_BLOCKS = "286:509:1303:1305:2319:3343:4806"
# Original NAND LCD frames in G9001's low-nibble-first 4bpp order.
FRAMES = {
    "ready": "abdec17f85f206db3954cd45ed266aec550337dc87757b00e6702780472b95a6",
    "a": "f10260b4615294c79efdd93bf2729630feabfa440b8119441bbc5e85e1fb4f0b",
    "ap": "d650704e5d5d6206cab613f5707048aac4fe1e090c81df116b45d7675773bd41",
    "app": "082fdb8d3fc8111dc9f854aa89a994dcd158420e8c28bf669ffbb1271f422f5d",
    "appl": "d532854cb4cbb5a9ff098bed90b29c8784fd29f128e90dfeecbefcababc38a34",
    "apple": "6a5d18e3c9eb0487c07de36c8051486c42cf80ea99a5bf060b96e4267ae30999",
    "down": "c4f29b797ee55e6858542663fce4b15113c3284cf54bbd15d47534977a6af129",
    "entry": "7981a08a4c44480c07124afdaa28fdf609191b7a9821ef336e9a1dc545815c27",
    "entry-audio-selected": "2f19630b76299342cf2cf060a8110f0a2f189b62da123ee957f2442184bf81c0",
    "card-menu": "7edb0d6fb64fe348c8cac26ca367ffdaca353094bb6b9f54173eba6dc470bbb0",
    "cj-ready": "3f27ed8ee6e8218ddd6aa330b86c8bbb8e91b1473b3bb7e34662b9ad920110f6",
    "ni-candidates": "a36215a4d5ce9bb73d0a1d522b334f3127914d594eb532387754088898cb2eae",
    "ni-entry": "4c9bfc276d93227a229aa10f7ea80fd57cba97ebfbd846d130e04f4fd81ff5fd",
    "power-off": "0e2476ee4a789fb3afdaf323ca140ca4eaa1728316cbaf8620a6253188565f0c",
}


class QMP:
    def __init__(self, stream):
        self.stream = stream
        self.read()
        self.command("qmp_capabilities")

    def read(self):
        line = self.stream.readline()
        if not line:
            raise RuntimeError("QEMU closed the QMP connection")
        return json.loads(line)

    def command(self, name, **arguments):
        request = {"execute": name, "arguments": arguments}
        self.stream.write(json.dumps(request).encode() + b"\n")
        self.stream.flush()
        while True:
            reply = self.read()
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError(reply["error"]["desc"])
            return reply["return"]


def connect(connection, address, process):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            connection.connect(str(address))
            return
        except (FileNotFoundError, ConnectionRefusedError):
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited with {process.returncode}") from None
            time.sleep(0.1)
    raise RuntimeError("QMP socket did not appear")


def wait_for_frame(qmp, path, expected, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        qmp.command("screendump", filename=str(path))
        if hashlib.sha256(path.read_bytes()).hexdigest() == FRAMES[expected]:
            return
        time.sleep(0.2)
    raise RuntimeError(f"Expected {expected!r} on the LCD. Last frame: {path}")


def send_key(qmp, key):
    qmp.command("send-key", keys=[{"type": "qcode", "data": key}], **{"hold-time": 170})


def press(qmp, out, key, name, expected, timeout=12):
    send_key(qmp, key)
    # The search task may still be reading dictionary data after a redraw.
    time.sleep(1.4)
    wait_for_frame(qmp, out / f"{name}.ppm", expected, timeout)
    print(f"PASS  {key} -> {expected}", flush=True)


def exercise(qmp, out):
    wait_for_frame(qmp, out / "00-ready.ppm", "ready", 90)

    # Give the search task time to finish initialization and return to HALT.
    time.sleep(2)
    press(qmp, out, "a", "01-a", "a")
    press(qmp, out, "p", "02-ap", "ap")
    press(qmp, out, "delete", "03-delete", "a")
    press(qmp, out, "esc", "04-escape", "ready")

    for count, key in enumerate("apple", 1):
        prefix = "apple"[:count]
        press(qmp, out, key, f"{count + 4:02d}-{prefix}", prefix)

    press(qmp, out, "down", "10-down", "down")
    press(qmp, out, "up", "11-up", "apple")
    press(qmp, out, "ret", "12-entry", "entry")


def exercise_card_lookup(qmp, out):
    wait_for_frame(qmp, out / "00-ready.ppm", "ready", 100)
    time.sleep(2)
    press(qmp, out, "f11", "01-card-menu", "card-menu", timeout=35)
    press(qmp, out, "ret", "02-cj-ready", "cj-ready", timeout=35)
    send_key(qmp, "n")
    # The first pinyin input loads the index and preview from the card.
    time.sleep(9)
    press(qmp, out, "i", "03-ni-candidates", "ni-candidates", timeout=60)
    press(qmp, out, "ret", "04-ni-entry", "ni-entry", timeout=40)


def exercise_power_cycle(qmp, out):
    exercise(qmp, out)
    press(qmp, out, "f12", "13-power-off", "power-off", timeout=30)
    time.sleep(3)
    press(qmp, out, "f12", "14-resumed-entry", "entry", timeout=100)


def exercise_pronunciation(qmp, out, clip, card_lookup):
    if not card_lookup:
        press(qmp, out, "ret", "13-audio-selected", "entry-audio-selected")
    send_key(qmp, "ret")

    deadline = time.monotonic() + 40
    while not clip.is_file():
        if time.monotonic() >= deadline:
            raise RuntimeError("The firmware produced no pronunciation audio")
        time.sleep(0.2)

    decoded = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-xerror", "-f", "mp3", "-i", str(clip),
         "-f", "s16le", "-"],
        stdout=subprocess.PIPE, check=True,
    ).stdout
    if not any(abs(sample) > 1024 for (sample,) in struct.iter_unpack("<h", decoded)):
        raise RuntimeError("The pronunciation has no audible speech")
    shutil.copyfile(clip, out / ("05-ni.mp3" if card_lookup else "14-apple.mp3"))
    print("PASS  firmware pronunciation decodes to non-silent PCM", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=QEMU)
    parser.add_argument("--nand", type=Path, required=True,
                        help="path to the original NAND dump")
    parser.add_argument("--card", type=Path, help="mount a dictionary card image")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--card-lookup", action="store_true",
                      help="look up ni on the original Chinese dictionary card")
    mode.add_argument("--power-cycle", action="store_true",
                      help="switch the firmware off and on with the panel key")
    parser.add_argument("--pronunciation", action="store_true",
                        help="decode the selected entry's original pronunciation")
    parser.add_argument("--out", type=Path, default=ROOT / "_build/g9001-keyboard-smoke")
    args = parser.parse_args()
    if args.card_lookup and args.card is None:
        parser.error("--card-lookup requires --card")
    if args.pronunciation and shutil.which("ffmpeg") is None:
        parser.error("--pronunciation requires ffmpeg")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    machine = f"g9001,nand-image={args.nand.resolve()},nand-bad-blocks={BAD_BLOCKS}"
    if args.card:
        machine += f",spi-card-type=dict,spi-image={args.card.resolve()}"

    with tempfile.TemporaryDirectory(prefix="g9001-qmp-") as tmp, \
            (out / "qemu.log").open("w") as log:
        address = Path(tmp) / "qmp"
        clip = Path(tmp) / "pronunciation.mp3"
        if args.pronunciation:
            machine += f",audio-output={clip}"
        command = [str(args.qemu.resolve()), "-M", machine,
                   "-cpu", "c33l05,exit-on-halt=off", "-display", "none",
                   "-serial", "none", "-monitor", "none",
                   "-qmp", f"unix:{address},server=on,wait=off"]
        with subprocess.Popen(command, stdout=log, stderr=log) as process:
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(5)
                    connect(connection, address, process)
                    with connection.makefile("rwb") as stream:
                        qmp = QMP(stream)
                        if args.card_lookup:
                            exercise_card_lookup(qmp, out)
                        elif args.power_cycle:
                            exercise_power_cycle(qmp, out)
                        else:
                            exercise(qmp, out)
                        if args.pronunciation:
                            exercise_pronunciation(qmp, out, clip, args.card_lookup)
                        qmp.command("quit")
                if process.wait(timeout=5) != 0:
                    raise RuntimeError(f"QEMU exited with {process.returncode}")
            finally:
                if process.poll() is None:
                    process.terminate()
    print(f"Frames: {out}")


if __name__ == "__main__":
    main()
