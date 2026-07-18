#!/usr/bin/env python3
"""
LR24-F transparent serial link tester.

Examples:
  Air-side Ubuntu:
    python3 lr24_link_test.py responder --port /dev/ttyUSB0

  Ground-side Ubuntu:
    python3 lr24_link_test.py initiator --port /dev/ttyUSB0 --count 200 --interval 0.5
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial is missing. Install it with: sudo apt install python3-serial", file=sys.stderr)
    raise SystemExit(2)


def open_serial(port: str, baud: int, timeout: float) -> serial.Serial:
    try:
        ser = serial.Serial(
            port=port,
            baudrate=baud,
            timeout=timeout,
            write_timeout=timeout,
        )
    except serial.SerialException as exc:
        print(f"Unable to open {port}: {exc}", file=sys.stderr)
        raise SystemExit(1)

    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def run_responder(args: argparse.Namespace) -> None:
    ser = open_serial(args.port, args.baud, args.timeout)
    print(f"Responder listening on {args.port} at {args.baud} bps. Press Ctrl+C to stop.")

    received = 0
    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            parts = line.split(",", 3)

            if len(parts) >= 3 and parts[0] == "PING":
                seq = parts[1]
                sent_ns = parts[2]
                reply = f"PONG,{seq},{sent_ns}\n".encode("ascii")
                ser.write(reply)
                ser.flush()
                received += 1
                print(f"RX seq={seq} bytes={len(raw)} -> ACK")
            else:
                print(f"RX unrecognized: {line!r}")
    except KeyboardInterrupt:
        print(f"\nStopped. Valid PING packets received: {received}")
    finally:
        ser.close()


def run_initiator(args: argparse.Namespace) -> None:
    ser = open_serial(args.port, args.baud, args.timeout)
    rtts_ms: list[float] = []
    lost = 0

    print(
        f"Sending {args.count} packets through {args.port}, "
        f"interval={args.interval}s, payload={args.payload} bytes."
    )

    try:
        for seq in range(1, args.count + 1):
            cycle_start = time.monotonic()
            sent_ns = time.monotonic_ns()
            payload = "X" * args.payload
            packet = f"PING,{seq},{sent_ns},{payload}\n".encode("ascii")

            ser.write(packet)
            ser.flush()

            deadline = time.monotonic() + args.timeout
            matched = False

            while time.monotonic() < deadline:
                raw = ser.readline()
                if not raw:
                    continue

                line = raw.decode("utf-8", errors="replace").strip()
                parts = line.split(",")

                if (
                    len(parts) >= 3
                    and parts[0] == "PONG"
                    and parts[1] == str(seq)
                    and parts[2] == str(sent_ns)
                ):
                    rtt_ms = (time.monotonic_ns() - sent_ns) / 1_000_000
                    rtts_ms.append(rtt_ms)
                    matched = True
                    print(f"seq={seq:04d}  RTT={rtt_ms:8.2f} ms")
                    break

            if not matched:
                lost += 1
                print(f"seq={seq:04d}  TIMEOUT")

            elapsed = time.monotonic() - cycle_start
            if seq < args.count and elapsed < args.interval:
                time.sleep(args.interval - elapsed)

    except KeyboardInterrupt:
        print("\nInterrupted by user.")
    finally:
        ser.close()

    sent = len(rtts_ms) + lost
    received = len(rtts_ms)
    loss_pct = (lost / sent * 100.0) if sent else 0.0

    print("\n=== LR24 Link Test Summary ===")
    print(f"Sent:       {sent}")
    print(f"Received:   {received}")
    print(f"Lost:       {lost} ({loss_pct:.2f}%)")

    if rtts_ms:
        print(f"RTT min:    {min(rtts_ms):.2f} ms")
        print(f"RTT average:{statistics.fmean(rtts_ms):.2f} ms")
        print(f"RTT median: {statistics.median(rtts_ms):.2f} ms")
        print(f"RTT p95:    {percentile(rtts_ms, 0.95):.2f} ms")
        print(f"RTT max:    {max(rtts_ms):.2f} ms")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Test an LR24-F transparent serial link.")
    subparsers = parser.add_subparsers(dest="mode", required=True)

    responder = subparsers.add_parser("responder", help="Receive PING and return PONG.")
    responder.add_argument("--port", required=True)
    responder.add_argument("--baud", type=int, default=115200)
    responder.add_argument("--timeout", type=float, default=0.2)
    responder.set_defaults(func=run_responder)

    initiator = subparsers.add_parser("initiator", help="Send PING packets and measure RTT/loss.")
    initiator.add_argument("--port", required=True)
    initiator.add_argument("--baud", type=int, default=115200)
    initiator.add_argument("--timeout", type=float, default=2.0)
    initiator.add_argument("--count", type=int, default=200)
    initiator.add_argument("--interval", type=float, default=0.5)
    initiator.add_argument("--payload", type=int, default=32)
    initiator.set_defaults(func=run_initiator)

    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if getattr(args, "payload", 0) < 0:
        raise SystemExit("--payload must be 0 or greater")
    if getattr(args, "count", 1) <= 0:
        raise SystemExit("--count must be greater than 0")
    args.func(args)


if __name__ == "__main__":
    main()
