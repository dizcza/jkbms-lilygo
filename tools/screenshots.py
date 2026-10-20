"""
Takes screenshots of the T-Display UI for the README.

Needs firmware built with CONFIG_UI_SCREENSHOT_CONSOLE=y and a BMS in range.
The board is reset, then driven page by page over the serial console.

    python tools/screenshots.py /dev/ttyACM0 docs/screenshots
"""

import sys
import time
from pathlib import Path

import serial
from PIL import Image

SCALE = 2


def wait_for(port, text, timeout):
    end = time.time() + timeout
    while time.time() < end:
        line = port.readline().decode(errors="replace")
        if text in line:
            return line
    raise TimeoutError(f"'{text}' not seen")


def command(port, keys, delay=0.6):
    for key in keys:
        port.write(key.encode())
        time.sleep(delay)


def screenshot(port, path):
    port.reset_input_buffer()
    port.write(b"s")
    header = wait_for(port, "SHOT", 10).split()
    if header[1] == "ERROR":
        raise RuntimeError("snapshot failed")
    width, height = int(header[1]), int(header[2])
    pixels = bytearray()
    while True:
        line = port.readline().decode(errors="replace").strip()
        if line == "END":
            break
        if len(line) == width * 4:
            pixels += bytes.fromhex(line)
    image = Image.new("RGB", (width, height))
    data = []
    for i in range(0, len(pixels), 2):
        value = pixels[i] | (pixels[i + 1] << 8)  # little-endian RGB565
        data.append((((value >> 11) & 0x1F) * 255 // 31, ((value >> 5) & 0x3F) * 255 // 63,
                     (value & 0x1F) * 255 // 31))
    image.putdata(data)
    image = image.resize((width * SCALE, height * SCALE), Image.NEAREST)
    image.save(path)
    print(f"saved {path}")


def main():
    port_name, out_dir = sys.argv[1], Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port_name, 115200, timeout=1)
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False  # reset into the app

    wait_for(port, "streaming:", 60)
    wait_for(port, "logbook:", 20)
    time.sleep(1)
    command(port, "a")  # made-up BMS names and MACs

    screenshot(port, out_dir / "summary.png")
    command(port, "n")
    screenshot(port, out_dir / "cells.png")
    command(port, "n")
    screenshot(port, out_dir / "pack.png")
    command(port, "n")
    screenshot(port, out_dir / "cell.png")
    command(port, "n" * 8)              # cells 2..8, then errors
    screenshot(port, out_dir / "errors.png")
    command(port, "n")
    screenshot(port, out_dir / "settings.png")
    command(port, "n" * 7)              # remaining settings pages, then the logbook
    screenshot(port, out_dir / "log.png")
    command(port, "z")
    screenshot(port, out_dir / "sleep.png")
    command(port, "Z")
    command(port, "L", delay=7)         # long press: the list shows after a 5 s search
    screenshot(port, out_dir / "pick.png")
    command(port, "n", delay=0.3)       # select: connects behind the spinner
    screenshot(port, out_dir / "connecting.png")


if __name__ == "__main__":
    main()
