#!/usr/bin/env python3
"""Verify all SR-G9001 panel keys through QMP input and GPIO reads."""

import argparse
from contextlib import ExitStack
from pathlib import Path
import socket
import subprocess
import tempfile

from keyboard_smoke import QEMU, QMP, connect


# Eight rows in the order returned by the original firmware's GPIO scanner.
KEYS = (
    "home f1 q o a z semicolon -",
    "f8 f2 w p s x apostrophe equal",
    "f9 f3 e l d c comma pgup",
    "tab f4 r minus f v dot pgdn",
    "insert f5 t slash g b esc left",
    "f12 f6 y bracket_left h n ret right",
    "f11 f7 u bracket_right j m delete up",
    "- f10 i backslash k - - down",
)
COLUMNS = ((1, 3), (4, 1), (4, 2), (4, 3),
           (3, 2), (3, 3), (3, 4), (3, 5))
PORT_DATA = 0x300380


def event(qmp, key, down):
    qmp.command("input-send-event", events=[{
        "type": "key",
        "data": {"down": down, "key": {"type": "qcode", "data": key}},
    }])


def bus_command(bus, command):
    bus.write(command.encode() + b"\n")
    bus.flush()
    answer = bus.readline().split()
    if not answer or answer[0] != b"OK":
        raise RuntimeError(f"qtest {command}: {answer!r}")
    return int(answer[1], 16) if len(answer) > 1 else None


def write(bus, address, value):
    bus_command(bus, f"writeb {address:#x} {value:#x}")


def scan(bus):
    columns = []
    for port, bit in COLUMNS:
        address = PORT_DATA + 2 * port
        write(bus, address, 0xff ^ (1 << bit))
        p6 = ~bus_command(bus, "readb 0x30038c")
        p7 = ~bus_command(bus, "readb 0x30038e")
        columns.append((p6 & 0x0f) | ((p7 & 7) << 4) | ((p6 & 0x10) << 3))
        write(bus, address, 0xff)
    return columns


def expect(bus, key, expected):
    actual = scan(bus)
    if actual != expected:
        raise RuntimeError(f"{key}: expected column masks {expected}, got {actual}")


def exercise(qmp, bus):
    for port, mask in ((1, 0x08), (3, 0x3c), (4, 0x0e)):
        write(bus, PORT_DATA + 2 * port, 0xff)
        write(bus, PORT_DATA + 2 * port + 1, mask)
    write(bus, PORT_DATA + 2 * 6 + 1, 0)
    write(bus, PORT_DATA + 2 * 7 + 1, 0)

    locations = {}
    released = [0] * 8
    for row, key_row in enumerate(KEYS):
        for col, key in enumerate(key_row.split()):
            if key == "-":
                continue
            pressed = released.copy()
            pressed[col] = 1 << row
            locations[key] = pressed
            event(qmp, key, True)
            expect(bus, key, pressed)
            event(qmp, key, False)
            expect(bus, f"release {key}", released)
    if len(locations) != 60:
        raise RuntimeError(f"expected 60 panel keys, tested {len(locations)}")
    print("PASS  all 60 panel keys close and release the expected matrix switch")

    aliases = {"backspace": "delete", "menu": "f10", "kp_enter": "ret",
               "kp_equals": "ret", "kp_decimal": "a", "kp_divide": "v",
               "kp_multiply": "b", "kp_subtract": "n", "kp_add": "m"}
    for number, letter in zip("1234567890", "qwertyuiop"):
        aliases[number] = letter
        aliases[f"kp_{number}"] = letter
    for alias, primary in aliases.items():
        event(qmp, alias, True)
        expect(bus, alias, locations[primary])
        event(qmp, primary, True)
        event(qmp, alias, False)
        expect(bus, f"release {alias} with {primary} held", locations[primary])
        event(qmp, primary, False)
        expect(bus, f"release {primary}", released)
    print(f"PASS  all {len(aliases)} alternate PC keys share the correct switch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=QEMU)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="g9001-keys-") as tmp:
        qmp_path = Path(tmp) / "qmp"
        bus_path = Path(tmp) / "qtest"
        command = [str(args.qemu.resolve()), "-machine", "g9001", "-accel",
                   "qtest", "-display", "none", "-monitor", "none", "-serial",
                   "none", "-qtest-log", "/dev/null", "-qmp",
                   f"unix:{qmp_path},server=on,wait=off", "-qtest",
                   f"unix:{bus_path},server=on,wait=off"]
        with subprocess.Popen(command, stderr=subprocess.PIPE, text=True) as process:
            try:
                with ExitStack() as stack:
                    streams = []
                    for address in (qmp_path, bus_path):
                        endpoint = stack.enter_context(socket.socket(socket.AF_UNIX))
                        endpoint.settimeout(5)
                        connect(endpoint, address, process)
                        streams.append(stack.enter_context(endpoint.makefile("rwb")))
                    qmp = QMP(streams[0])
                    exercise(qmp, streams[1])
                    qmp.command("quit")
                if process.wait(timeout=5) != 0:
                    raise RuntimeError(process.stderr.read())
            finally:
                if process.poll() is None:
                    process.terminate()


if __name__ == "__main__":
    main()
