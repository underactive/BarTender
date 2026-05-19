#!/usr/bin/env python3
"""Save a screenshot from the CodexBar desk toy over USB serial.

Usage:
    python3 scripts/screenshot.py [port] [output.png]

With no arguments the script auto-detects the first /dev/cu.usbmodem* or
/dev/cu.usbserial* port and writes screenshot_YYYYMMDD_HHMMSS.png.

Requirements:
    pip3 install pyserial Pillow
"""

import sys
import struct
import time
from datetime import datetime

MAGIC = b"SCAP"
BAUD  = 115200   # irrelevant for USB-JTAG but required by pyserial


def find_port():
    try:
        import serial.tools.list_ports
    except ImportError:
        return None
    candidates = [p.device for p in serial.tools.list_ports.comports()
                  if "usbmodem" in p.device or "usbserial" in p.device]
    if not candidates:
        return None
    # Prefer /dev/cu.* (call-out) over /dev/tty.* (macOS convention)
    cu = [p for p in candidates if "/cu." in p]
    return cu[0] if cu else candidates[0]


def scan_for_magic(ser):
    """Read bytes until the 4-byte SCAP magic appears in the stream."""
    buf = b""
    for _ in range(128 * 1024):   # bail after 128 KB of noise
        b = ser.read(1)
        if not b:
            raise TimeoutError("Timed out waiting for SCAP magic")
        buf += b
        if buf.endswith(MAGIC):
            return
    raise ValueError("SCAP magic not found after 128 KB — check device/port")


def receive_frame(ser):
    """Scan for magic, read header, then stream pixel data."""
    scan_for_magic(ser)

    # Header immediately after magic: w(u16le) h(u16le) length(u32le)
    hdr = ser.read(8)
    if len(hdr) < 8:
        raise ValueError(f"Truncated header ({len(hdr)} bytes)")
    w, h, data_len = struct.unpack("<HHI", hdr)
    print(f"  Frame: {w}×{h}, {data_len:,} bytes", flush=True)

    chunks = []
    received = 0
    while received < data_len:
        chunk = ser.read(min(data_len - received, 4096))
        if not chunk:
            raise TimeoutError(
                f"Stalled: received {received}/{data_len} bytes")
        chunks.append(chunk)
        received += len(chunk)
        sys.stdout.write(f"\r  Progress: {received:,}/{data_len:,} bytes")
        sys.stdout.flush()
    print()

    return w, h, b"".join(chunks)


def rgb565_to_png(data, w, h, outfile):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow not found — run: pip3 install Pillow")

    pixels = []
    for i in range(0, w * h * 2, 2):
        word = struct.unpack_from("<H", data, i)[0]
        # RGB565-LE: R[15:11] G[10:5] B[4:0]
        r = ((word >> 11) & 0x1F) << 3
        g = ((word >>  5) & 0x3F) << 2
        b = ( word        & 0x1F) << 3
        pixels.append((r, g, b))

    img = Image.new("RGB", (w, h))
    img.putdata(pixels)
    img.save(outfile)
    print(f"  Saved  → {outfile}")


def main():
    try:
        import serial
    except ImportError:
        sys.exit("pyserial not found — run: pip3 install pyserial")

    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        sys.exit(
            "No device found.\n"
            "  Connect the board via USB, or pass the port explicitly:\n"
            "  python3 scripts/screenshot.py /dev/cu.usbmodem14101"
        )

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    outfile = sys.argv[2] if len(sys.argv) > 2 else f"screenshot_{ts}.png"

    print(f"Port:   {port}")
    print(f"Output: {outfile}")

    with serial.Serial(port, BAUD, timeout=20) as ser:
        time.sleep(0.1)             # let the port settle after open
        ser.reset_input_buffer()
        ser.write(b"screenshot\n")
        ser.flush()
        print("Command sent — waiting for response…")
        w, h, data = receive_frame(ser)

    rgb565_to_png(data, w, h, outfile)


if __name__ == "__main__":
    main()
