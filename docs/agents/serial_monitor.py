#!/usr/bin/env python3
"""ESP32 serial monitor with reset for non-interactive (AI agent) environments.

Usage:
    # Must source ESP-IDF first (for pyserial):
    source ~/.espressif/v6.0/esp-idf/export.sh
    python3 docs/agents/serial_monitor.py --timeout 20
    python3 docs/agents/serial_monitor.py --timeout 15 --port /dev/cu.usbserial-130
    python3 docs/agents/serial_monitor.py --timeout 10 --no-reset

Options:
    --timeout SECONDS   How long to read serial after reset (default: 15)
    --port   PORT       Serial port (default: auto-detect first cu.usb*)
    --baud   RATE       Baud rate (default: 115200)
    --no-reset          Skip hardware reset, just read current output
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not found. Run: source ~/.espressif/v6.0/esp-idf/export.sh", file=sys.stderr)
    sys.exit(1)


def auto_detect_port():
    ports = sorted(glob.glob("/dev/cu.usb*"))
    if not ports:
        print("ERROR: No serial port found. ls /dev/cu.usb* returned nothing.", file=sys.stderr)
        sys.exit(1)
    return ports[0]


def hardware_reset(ser):
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(description="ESP32 serial monitor for non-interactive environments")
    parser.add_argument("--timeout", type=float, default=15, help="Read duration in seconds (default: 15)")
    parser.add_argument("--port", default=None, help="Serial port (default: auto-detect)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--no-reset", action="store_true", help="Skip hardware reset, just read")
    args = parser.parse_args()

    port = args.port or auto_detect_port()

    try:
        ser = serial.Serial(port=port, baudrate=args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {port}: {e}", file=sys.stderr)
        sys.exit(1)

    if not args.no_reset:
        hardware_reset(ser)

    print(f"[serial_monitor] Reading {port} for {args.timeout}s ...", file=sys.stderr)
    start = time.time()
    try:
        while time.time() - start < args.timeout:
            data = ser.readline()
            if data:
                try:
                    line = data.decode("utf-8", errors="replace").rstrip("\n\r")
                    print(line)
                    sys.stdout.flush()
                except Exception:
                    pass
    except KeyboardInterrupt:
        print("\n[serial_monitor] Interrupted.", file=sys.stderr)
    finally:
        ser.close()
        print(f"[serial_monitor] Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
