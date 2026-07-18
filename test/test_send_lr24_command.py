from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "tools" / "send_lr24_command.py"
SPEC = importlib.util.spec_from_file_location("send_lr24_command", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
lr24 = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = lr24
SPEC.loader.exec_module(lr24)


def response_frame(
    frame_type: str,
    sequence: str,
    command: str,
    message: str,
) -> bytes:
    payload = f"{frame_type},{sequence},{command},{message}"
    return f"${payload}*{lr24.checksum(payload):02X}\n".encode("ascii")


class FakeSerial:
    def __init__(self, lines: list[bytes]) -> None:
        self._lines = iter(lines)

    def readline(self) -> bytes:
        return next(self._lines, b"")


class FramingTests(unittest.TestCase):
    def test_legacy_no_argument_frame_is_unchanged(self) -> None:
        payload = "CMD,123,START_OFFBOARD"
        self.assertEqual(
            lr24.build_frame("123", "start_offboard"),
            f"${payload}*{lr24.checksum(payload):02X}\n",
        )

    def test_goto_relative_frame_has_exact_fields(self) -> None:
        payload = "CMD,41,GOTO,24.786,120.997,80"
        self.assertEqual(
            lr24.build_frame("41", "goto", "24.786", "120.997", "80"),
            f"${payload}*{lr24.checksum(payload):02X}\n",
        )

    def test_goto_amsl_accepts_argument_sequence(self) -> None:
        payload = "CMD,42,GOTO_AMSL,-35.1,179.9,123.5"
        self.assertEqual(
            lr24.build_frame(
                "42", "GOTO_AMSL", ["-35.1", "179.9", "123.5"]
            ),
            f"${payload}*{lr24.checksum(payload):02X}\n",
        )

    def test_goto_requires_exactly_three_arguments(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires latitude"):
            lr24.build_frame("1", "GOTO", "24", "121")
        with self.assertRaisesRegex(ValueError, "requires latitude"):
            lr24.build_frame("1", "GOTO", "24", "121", "50", "extra")

    def test_non_goto_command_rejects_arguments(self) -> None:
        with self.assertRaisesRegex(ValueError, "does not accept arguments"):
            lr24.build_frame("1", "PING", "extra")

    def test_simple_mode_rejects_flight_critical_commands(self) -> None:
        for command in (
            "ENABLE_STREAM",
            "START_MISSION",
            "START_OFFBOARD",
            "STOP_OFFBOARD",
            "LAND",
            "GOTO",
            "GOTO_AMSL",
            "RTL",
            "ABORT",
        ):
            with self.subTest(command=command):
                with self.assertRaisesRegex(ValueError, "checksummed frame"):
                    if command.startswith("GOTO"):
                        lr24.build_simple_command(command, "24", "121", "50")
                    else:
                        lr24.build_simple_command(command)

    def test_simple_mode_preserves_legacy_ping(self) -> None:
        self.assertEqual(lr24.build_simple_command("ping"), "PING\n")


class ValidationTests(unittest.TestCase):
    def test_coordinates_must_be_finite_decimal_numbers(self) -> None:
        invalid_values = ("nan", "inf", "-inf", "0x1p2", " 24", "24 ", "1e")
        for value in invalid_values:
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "finite decimal"):
                    lr24.build_frame("1", "GOTO", value, "121", "50")

    def test_coordinate_ranges_are_checked(self) -> None:
        with self.assertRaisesRegex(ValueError, "latitude"):
            lr24.build_frame("1", "GOTO", "90.0001", "121", "50")
        with self.assertRaisesRegex(ValueError, "longitude"):
            lr24.build_frame("1", "GOTO", "24", "-180.001", "50")

    def test_altitude_must_fit_service_float32(self) -> None:
        with self.assertRaisesRegex(ValueError, "float32"):
            lr24.build_frame("1", "GOTO_AMSL", "24", "121", "3.5e38")

    def test_sequence_rejects_reserved_characters(self) -> None:
        for sequence in ("", "1,2", "1*2", "1\n2", "1\t2"):
            with self.subTest(sequence=sequence):
                with self.assertRaisesRegex(ValueError, "sequence"):
                    lr24.build_frame(sequence, "PING")

    def test_cli_default_timeout_covers_global_confirmation(self) -> None:
        parsed = lr24._make_parser().parse_args(["PING"])
        self.assertGreaterEqual(parsed.timeout, 7.0)


class ResponseParsingTests(unittest.TestCase):
    def test_parse_valid_ack(self) -> None:
        parsed = lr24.parse_response_frame(
            response_frame("ACK", "123", "GOTO", "target accepted")
        )
        self.assertEqual(parsed.frame_type, "ACK")
        self.assertEqual(parsed.sequence, "123")
        self.assertEqual(parsed.command, "GOTO")
        self.assertEqual(parsed.message, "target accepted")

    def test_parse_rejects_bad_checksum(self) -> None:
        with self.assertRaisesRegex(ValueError, "checksum"):
            lr24.parse_response_frame("$ACK,1,PING,PONG*00\n")

    def test_parse_rejects_trailing_text(self) -> None:
        valid = response_frame("ACK", "1", "PING", "PONG").decode("ascii")
        with self.assertRaisesRegex(ValueError, "exactly two"):
            lr24.parse_response_frame(valid.rstrip("\n") + "junk")

    def test_wait_skips_unrelated_invalid_and_status_frames(self) -> None:
        serial_port = FakeSerial(
            [
                b"noise\n",
                response_frame("ACK", "11", "PING", "PONG"),
                response_frame("STAT", "42", "BOOT", "ready"),
                b"$ACK,42,GOTO,bad*00\n",
                response_frame("ACK", "42", "GOTO", "target accepted"),
            ]
        )

        parsed = lr24.wait_for_matching_response(serial_port, "42", 0.5)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.sequence, "42")
        self.assertEqual(parsed.message, "target accepted")


if __name__ == "__main__":
    unittest.main()
