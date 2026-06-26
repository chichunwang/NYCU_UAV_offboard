#!/usr/bin/env python3
import argparse
import itertools
import time

import serial


def checksum(payload: str) -> int:
    value = 0
    for byte in payload.encode("ascii"):
        value ^= byte
    return value


def build_frame(sequence: str, command: str) -> str:
    payload = f"CMD,{sequence},{command.upper()}"
    return f"${payload}*{checksum(payload):02X}\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Send one LR24-F command frame.")
    parser.add_argument("command", help="PING, ENABLE_STREAM, START_MISSION, START_OFFBOARD, STOP_OFFBOARD, LAND, STATUS")
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seq", default=None)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--simple", action="store_true", help="Send COMMAND\\n without framing or checksum.")
    args = parser.parse_args()

    sequence = args.seq or str(int(time.time()) % 100000)
    frame = f"{args.command.upper()}\n" if args.simple else build_frame(sequence, args.command)

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.write(frame.encode("ascii"))
        ser.flush()
        print(f"> {frame.strip()}")

        deadline = time.time() + args.timeout
        for _ in itertools.count():
            if time.time() >= deadline:
                print("No response before timeout.")
                return

            line = ser.readline()
            if line:
                print(f"< {line.decode(errors='replace').strip()}")
                return


if __name__ == "__main__":
    main()
