#!/usr/bin/env python3
import argparse
import sys
import time
from datetime import datetime

try:
    import serial
except ImportError as exc:
    raise SystemExit(f"pyserial is required: {exc}") from exc


def stamp() -> str:
    return datetime.now().isoformat(timespec="seconds")


def main() -> int:
    parser = argparse.ArgumentParser(description="Tail WeClawBot USB serial logs.")
    parser.add_argument("port", nargs="?", default="/dev/cu.usbmodem21101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--reopen-delay", type=float, default=1.5)
    args = parser.parse_args()

    while True:
        try:
            ser = serial.Serial()
            ser.port = args.port
            ser.baudrate = args.baud
            ser.timeout = 0.5
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            print(f"{stamp()} logger connected port={args.port}", flush=True)
            while True:
                line = ser.readline()
                if not line:
                    continue
                text = line.decode("utf-8", "replace").rstrip()
                print(f"{stamp()} {text}", flush=True)
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"{stamp()} logger error: {exc}", file=sys.stderr, flush=True)
            time.sleep(args.reopen_delay)


if __name__ == "__main__":
    raise SystemExit(main())
