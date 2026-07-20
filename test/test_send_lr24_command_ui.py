from __future__ import annotations

from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest import mock


TOOLS_PATH = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_PATH))

import send_lr24_command_ui as ui  # noqa: E402


def response_frame(
    frame_type: str,
    sequence: str,
    command: str,
    message: str,
) -> bytes:
    payload = f"{frame_type},{sequence},{command},{message}"
    return f"${payload}*{ui.checksum(payload):02X}\n".encode("ascii")


class FakeSerialPort:
    def __init__(self, response: bytes) -> None:
        self.response = response
        self.written = b""
        self.flushed = False

    def __enter__(self) -> "FakeSerialPort":
        return self

    def __exit__(self, *args: object) -> None:
        return None

    def write(self, data: bytes) -> None:
        self.written += data

    def flush(self) -> None:
        self.flushed = True

    def readline(self) -> bytes:
        response, self.response = self.response, b""
        return response


class CommandSenderTests(unittest.TestCase):
    def test_send_command_uses_shared_framing_and_response_parser(self) -> None:
        port = FakeSerialPort(response_frame("ACK", "123", "PING", "PONG"))
        serial_factory = mock.Mock(return_value=port)
        sent_frames: list[str] = []

        with (
            mock.patch.object(ui.time, "time_ns", return_value=123),
            mock.patch.object(
                ui,
                "serial",
                SimpleNamespace(Serial=serial_factory),
            ),
        ):
            result = ui.send_command(
                "COM10",
                115200,
                8.0,
                "PING",
                on_sent=sent_frames.append,
            )

        serial_factory.assert_called_once_with("COM10", 115200, timeout=0.2)
        self.assertEqual(port.written, result.frame.encode("ascii"))
        self.assertTrue(port.flushed)
        self.assertEqual(sent_frames, [result.frame])
        self.assertIsNotNone(result.response)
        assert result.response is not None
        self.assertEqual(result.response.frame_type, "ACK")
        self.assertEqual(result.response.message, "PONG")

    def test_send_command_validates_goto_before_opening_port(self) -> None:
        serial_factory = mock.Mock()
        with mock.patch.object(
            ui,
            "serial",
            SimpleNamespace(Serial=serial_factory),
        ):
            with self.assertRaisesRegex(ValueError, "latitude"):
                ui.send_command(
                    "COM10",
                    115200,
                    8.0,
                    "GOTO",
                    ("91", "121", "80"),
                )
        serial_factory.assert_not_called()

    def test_format_response_includes_valid_checksum(self) -> None:
        response = ui.ResponseFrame("ERR", "7", "GOTO", "not ready")
        formatted = ui.format_response(response)
        self.assertEqual(ui.ResponseFrame, type(response))
        self.assertTrue(formatted.startswith("$ERR,7,GOTO,not ready*"))
        self.assertEqual(len(formatted.rsplit("*", 1)[1]), 2)


if __name__ == "__main__":
    unittest.main()
