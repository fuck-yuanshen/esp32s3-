"""Capture UART0 diagnostics without asserting the board's reset lines."""

import argparse
import sys
import time
from pathlib import Path

import serial


def capture(port: str, baud: int, seconds: float, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with serial.Serial(port=None, baudrate=baud, timeout=0.2) as link:
        link.dtr = False
        link.rts = False
        link.port = port
        link.open()
        print(f"Listening on {port} at {baud}, log: {output}", flush=True)
        deadline = time.monotonic() + seconds
        with output.open("ab", buffering=0) as log:
            while time.monotonic() < deadline:
                data = link.read(link.in_waiting or 1)
                if data:
                    log.write(data)
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM24")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    try:
        capture(args.port, args.baud, args.seconds, args.output)
    except KeyboardInterrupt:
        pass
    except serial.SerialException as exc:
        parser.exit(1, f"Serial capture failed: {exc}\n")


if __name__ == "__main__":
    main()
