#!/usr/bin/env python3
"""Keep a watch serial console log while surviving device resets.

The ESP32-S3 exposes one USB CDC port. A panic or a reflash makes that port
disappear for a moment, so this script reopens it and keeps appending to the
same file. Each (re)connect is marked with a wall-clock line, which makes it
trivial to see when the device rebooted and why (the panic backtrace stays in
the file).

Usage:
    python3 scripts/watch_serial_log.py                 # auto port, logs/tee
    python3 scripts/watch_serial_log.py -o /tmp/watch.log -p /dev/cu.usbmodem2101
"""

from __future__ import annotations

import argparse
import glob
import sys
import time
from datetime import datetime

try:
    import serial  # pyserial, shipped with the ESP-IDF python env
except ImportError:  # pragma: no cover
    sys.exit("pyserial missing: run this with the ESP-IDF python environment")


def find_port() -> str:
    for pattern in ("/dev/cu.usbmodem*", "/dev/cu.wchusbserial*", "/dev/cu.SLAB_USBtoUART*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    raise SystemExit("no ESP32 serial port found; pass --port")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-p", "--port", default=None, help="serial device (auto-detected)")
    parser.add_argument("-o", "--output", default="/tmp/watch-serial.log", help="log file")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    args = parser.parse_args()

    sys.stdout.write(f"capturing to {args.output} (Ctrl+C to stop)\n")
    sys.stdout.flush()

    while True:
        port = args.port or find_port()
        try:
            connection = serial.Serial(port, args.baud, timeout=0.5)
        except Exception as error:  # port still re-enumerating
            time.sleep(0.5)
            continue

        with open(args.output, "ab", buffering=0) as log:
            stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log.write(f"\n===== {stamp} attached {port} =====\n".encode())
            pending = b""
            try:
                while True:
                    chunk = connection.read(4096)
                    if not chunk:
                        continue
                    log.write(chunk)
                    pending += chunk
                    # Echo complete lines to stdout so a foreground run is usable.
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        sys.stdout.write(line.decode("utf-8", "replace").rstrip() + "\n")
                        sys.stdout.flush()
            except (serial.SerialException, OSError):
                stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                with open(args.output, "ab", buffering=0) as marker:
                    marker.write(f"\n===== {stamp} device disappeared =====\n".encode())
                try:
                    connection.close()
                except Exception:
                    pass
                time.sleep(0.5)
            except KeyboardInterrupt:
                try:
                    connection.close()
                except Exception:
                    pass
                sys.stdout.write("\nstopped\n")
                return


if __name__ == "__main__":
    main()
