#!/usr/bin/env python3
"""Send one checksummed command over the LR24-F transparent serial link."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import math
import re
import time
from typing import Iterable, Sequence

try:
    import serial
except ImportError:  # Keep framing helpers importable on machines without pyserial.
    serial = None  # type: ignore[assignment]


GOTO_COMMANDS = frozenset({"GOTO", "GOTO_AMSL"})
SIMPLE_FORBIDDEN_COMMANDS = frozenset(
    {
        "ENABLE_STREAM",
        "START_MISSION",
        "START_OFFBOARD",
        "STOP_OFFBOARD",
        "LAND",
        "GOTO",
        "GOTO_AMSL",
        "RTL",
        "ABORT",
    }
)
RESPONSE_TYPES = frozenset({"ACK", "ERR", "STAT"})
FLOAT32_MAX = 3.4028234663852886e38
DECIMAL_NUMBER = re.compile(
    r"^[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$"
)
HEX_BYTE = re.compile(r"^[0-9A-Fa-f]{2}$")


@dataclass(frozen=True)
class ResponseFrame:
    frame_type: str
    sequence: str
    command: str
    message: str


def checksum(payload: str) -> int:
    """Return the protocol XOR checksum for an ASCII payload."""
    value = 0
    for byte in payload.encode("ascii"):
        value ^= byte
    return value


def _validate_sequence(sequence: object) -> str:
    text = str(sequence)
    if not text:
        raise ValueError("sequence must not be empty")
    if any(character in text for character in ",*$\r\n"):
        raise ValueError("sequence contains a reserved framing character")
    if any(ord(character) < 0x20 or ord(character) > 0x7E for character in text):
        raise ValueError("sequence must contain printable ASCII characters only")
    try:
        text.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("sequence must contain ASCII characters only") from exc
    return text


def _normalize_command(command: object) -> str:
    text = str(command).strip().upper()
    if not text:
        raise ValueError("command must not be empty")
    if any(character in text for character in ",*$\r\n"):
        raise ValueError("command contains a reserved framing character")
    if any(ord(character) < 0x20 or ord(character) > 0x7E for character in text):
        raise ValueError("command must contain printable ASCII characters only")
    try:
        text.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("command must contain ASCII characters only") from exc
    return text


def _flatten_arguments(arguments: Sequence[object]) -> tuple[object, ...]:
    """Also accept build_frame(seq, cmd, [arg1, arg2, arg3]) for convenience."""
    if len(arguments) == 1 and isinstance(arguments[0], (list, tuple)):
        return tuple(arguments[0])
    return tuple(arguments)


def _parse_decimal(value: object, field_name: str) -> tuple[str, float]:
    text = str(value)
    if not DECIMAL_NUMBER.fullmatch(text):
        raise ValueError(f"{field_name} must be a finite decimal number")

    number = float(text)
    if not math.isfinite(number):
        raise ValueError(f"{field_name} must be a finite decimal number")
    return text, number


def validate_command_arguments(
    command: object,
    arguments: Sequence[object] = (),
) -> tuple[str, tuple[str, ...]]:
    """Validate a command and return its uppercase name and encoded arguments."""
    normalized_command = _normalize_command(command)
    flattened = _flatten_arguments(arguments)

    if normalized_command not in GOTO_COMMANDS:
        if flattened:
            raise ValueError(f"{normalized_command} does not accept arguments")
        return normalized_command, ()

    if len(flattened) != 3:
        altitude_name = (
            "altitude relative to home"
            if normalized_command == "GOTO"
            else "AMSL altitude"
        )
        raise ValueError(
            f"{normalized_command} requires latitude, longitude, and {altitude_name}"
        )

    latitude_text, latitude = _parse_decimal(flattened[0], "latitude")
    longitude_text, longitude = _parse_decimal(flattened[1], "longitude")
    altitude_text, altitude = _parse_decimal(flattened[2], "altitude")

    if not -90.0 <= latitude <= 90.0:
        raise ValueError("latitude must be in [-90,90]")
    if not -180.0 <= longitude <= 180.0:
        raise ValueError("longitude must be in [-180,180]")
    if abs(altitude) > FLOAT32_MAX:
        raise ValueError("altitude is outside float32 range")

    return normalized_command, (latitude_text, longitude_text, altitude_text)


def build_frame(sequence: object, command: object, *arguments: object) -> str:
    """Build a formal command frame, preserving the legacy two-argument API."""
    normalized_sequence = _validate_sequence(sequence)
    normalized_command, encoded_arguments = validate_command_arguments(command, arguments)

    fields = ["CMD", normalized_sequence, normalized_command, *encoded_arguments]
    payload = ",".join(fields)
    return f"${payload}*{checksum(payload):02X}\n"


def build_simple_command(command: object, *arguments: object) -> str:
    """Build legacy unframed text, excluding flight-critical commands."""
    normalized_command, encoded_arguments = validate_command_arguments(command, arguments)
    if normalized_command in SIMPLE_FORBIDDEN_COMMANDS:
        raise ValueError(
            f"{normalized_command} requires a checksummed frame; remove --simple"
        )
    if encoded_arguments:
        raise ValueError("simple commands cannot contain arguments")
    return f"{normalized_command}\n"


def parse_response_frame(line: str | bytes) -> ResponseFrame:
    """Parse and checksum-validate one ACK, ERR, or STAT response frame."""
    if isinstance(line, bytes):
        try:
            text = line.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("response is not ASCII") from exc
    else:
        text = line

    text = text.rstrip("\r\n")
    if not text.startswith("$"):
        raise ValueError("response does not start with '$'")

    star = text.find("*")
    if star < 0:
        raise ValueError("response is missing checksum")
    if star + 3 != len(text):
        raise ValueError("response checksum must be exactly two hex digits")

    checksum_text = text[star + 1 :]
    if not HEX_BYTE.fullmatch(checksum_text):
        raise ValueError("response checksum is not hexadecimal")

    payload = text[1:star]
    try:
        actual_checksum = checksum(payload)
    except UnicodeEncodeError as exc:
        raise ValueError("response payload is not ASCII") from exc
    expected_checksum = int(checksum_text, 16)
    if actual_checksum != expected_checksum:
        raise ValueError(
            f"bad response checksum; expected {actual_checksum:02X}"
        )

    fields = payload.split(",", 3)
    if len(fields) != 4:
        raise ValueError("expected TYPE,seq,command,message response payload")
    if fields[0] not in RESPONSE_TYPES:
        raise ValueError(f"unsupported response type {fields[0]!r}")
    if not fields[1]:
        raise ValueError("response sequence must not be empty")

    return ResponseFrame(
        frame_type=fields[0],
        sequence=fields[1],
        command=fields[2],
        message=fields[3],
    )


def wait_for_matching_response(
    serial_port: object,
    sequence: str,
    timeout: float,
) -> ResponseFrame | None:
    """Skip malformed/unrelated frames until this sequence receives ACK or ERR."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = serial_port.readline()
        if not raw:
            continue

        try:
            response = parse_response_frame(raw)
        except (TypeError, ValueError):
            continue

        if response.frame_type not in {"ACK", "ERR"}:
            continue
        if response.sequence != sequence:
            continue
        return response

    return None


def _make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send one LR24-F command frame.")
    parser.add_argument(
        "command",
        help=(
            "PING, ENABLE_STREAM, START_MISSION, START_OFFBOARD, "
            "STOP_OFFBOARD, LAND, RTL, ABORT, GOTO, GOTO_AMSL, or STATUS"
        ),
    )
    parser.add_argument(
        "arguments",
        nargs="*",
        metavar="ARG",
        help="For GOTO/GOTO_AMSL: latitude longitude altitude_m",
    )
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seq", default=None)
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="Response timeout in seconds (default: 8.0).",
    )
    parser.add_argument(
        "--simple",
        action="store_true",
        help="Send legacy COMMAND\\n; all flight-affecting commands are forbidden.",
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = _make_parser()
    args = parser.parse_args(argv)

    if args.timeout <= 0.0 or not math.isfinite(args.timeout):
        parser.error("--timeout must be a finite value greater than zero")
    if args.baud <= 0:
        parser.error("--baud must be greater than zero")

    # Keep the complete nanosecond timestamp. Truncating it to the sub-second remainder
    # could reuse a cached sequence for commands sent an exact number of seconds apart.
    sequence = (
        args.seq
        if args.seq is not None
        else str(time.time_ns())
    )
    try:
        if args.simple:
            frame = build_simple_command(args.command, *args.arguments)
            response_sequence = "0"
        else:
            frame = build_frame(sequence, args.command, *args.arguments)
            response_sequence = _validate_sequence(sequence)
    except ValueError as exc:
        parser.error(str(exc))

    if serial is None:
        parser.error("pyserial is missing; install it with: sudo apt install python3-serial")

    with serial.Serial(args.port, args.baud, timeout=0.2) as serial_port:
        serial_port.write(frame.encode("ascii"))
        serial_port.flush()
        print(f"> {frame.strip()}")

        response = wait_for_matching_response(
            serial_port,
            response_sequence,
            args.timeout,
        )
        if response is None:
            print("No matching checksummed response before timeout.")
            return 1

        response_payload = ",".join(
            [
                response.frame_type,
                response.sequence,
                response.command,
                response.message,
            ]
        )
        response_text = (
            f"${response_payload}*{checksum(response_payload):02X}"
        )
        print(f"< {response_text}")
        return 0 if response.frame_type == "ACK" else 1


if __name__ == "__main__":
    raise SystemExit(main())
