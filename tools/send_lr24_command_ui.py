#!/usr/bin/env python3
"""Graphical ground-station command sender for the LR24-F serial link."""

from __future__ import annotations

from dataclasses import dataclass
import math
import queue
import threading
import time
from typing import Callable, Sequence

try:
    import tkinter as tk
    from tkinter import messagebox, scrolledtext, ttk
except ImportError:  # pragma: no cover - depends on the host Python installation.
    tk = None  # type: ignore[assignment]
    messagebox = None  # type: ignore[assignment]
    scrolledtext = None  # type: ignore[assignment]
    ttk = None  # type: ignore[assignment]

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # Keep the module importable so main() can show a clear error.
    serial = None  # type: ignore[assignment]
    list_ports = None  # type: ignore[assignment]

from send_lr24_command import (
    ResponseFrame,
    build_frame,
    checksum,
    wait_for_matching_response,
)


BAUD_RATES = (57600, 115200)
DEFAULT_BAUD_RATE = 115200
DEFAULT_TIMEOUT = 2.0
FLIGHT_COMMANDS = frozenset(
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


@dataclass(frozen=True)
class SendResult:
    frame: str
    response: ResponseFrame | None


def format_response(response: ResponseFrame) -> str:
    """Recreate a response frame for display in the event log."""
    payload = ",".join(
        [
            response.frame_type,
            response.sequence,
            response.command,
            response.message,
        ]
    )
    return f"${payload}*{checksum(payload):02X}"


def available_serial_ports() -> list[str]:
    """Return serial device names in the platform's preferred order."""
    if list_ports is None:
        return []
    return sorted(port.device for port in list_ports.comports())


def send_command(
    port: str,
    baud_rate: int,
    timeout: float,
    command: str,
    arguments: Sequence[str] = (),
    *,
    on_sent: Callable[[str], None] | None = None,
) -> SendResult:
    """Open the port, send one checksummed command, and wait for ACK or ERR."""
    normalized_port = port.strip()
    if not normalized_port:
        raise ValueError("請選擇或輸入 serial port")
    if baud_rate <= 0:
        raise ValueError("Baud rate 必須大於 0")
    if timeout <= 0.0 or not math.isfinite(timeout):
        raise ValueError("Timeout 必須是大於 0 的有限數值")

    sequence = str(time.time_ns())
    frame = build_frame(sequence, command, *arguments)

    if serial is None:
        raise RuntimeError(
            "找不到 pyserial；請執行 'py -m pip install pyserial' "
            "或 'python3 -m pip install pyserial'"
        )

    with serial.Serial(normalized_port, baud_rate, timeout=0.2) as serial_port:
        serial_port.write(frame.encode("ascii"))
        serial_port.flush()
        if on_sent is not None:
            on_sent(frame)
        response = wait_for_matching_response(serial_port, sequence, timeout)

    return SendResult(frame=frame, response=response)


if tk is not None:

    class LR24CommandApp(tk.Tk):
        """Tk application that exposes LR24 commands as guarded buttons."""

        def __init__(self) -> None:
            super().__init__()
            self.title("LR24-F 指令控制台")
            self.geometry("920x700")
            self.minsize(780, 620)

            self.port_var = tk.StringVar()
            self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD_RATE))
            self.timeout_var = tk.StringVar(value=str(DEFAULT_TIMEOUT))
            self.latitude_var = tk.StringVar()
            self.longitude_var = tk.StringVar()
            self.altitude_var = tk.StringVar()
            self.status_var = tk.StringVar(value="就緒")
            self._events: queue.Queue[tuple[str, object]] = queue.Queue()
            self._command_buttons: list[ttk.Button] = []
            self._busy = False

            self._configure_styles()
            self._build_layout()
            self._refresh_ports(log_result=False)
            self.after(100, self._process_events)

        def _configure_styles(self) -> None:
            style = ttk.Style(self)
            style.configure("Command.TButton", padding=(10, 7))
            style.configure(
                "Danger.TButton",
                padding=(10, 7),
                foreground="#b91c1c",
            )
            style.configure("Status.TLabel", padding=(6, 4))

        def _build_layout(self) -> None:
            self.columnconfigure(0, weight=1)
            self.rowconfigure(3, weight=1)

            connection = ttk.LabelFrame(self, text="連線設定", padding=10)
            connection.grid(row=0, column=0, sticky="ew", padx=12, pady=(12, 6))
            connection.columnconfigure(1, weight=1)

            ttk.Label(connection, text="Serial port").grid(
                row=0, column=0, sticky="w", padx=(0, 6)
            )
            self.port_combo = ttk.Combobox(
                connection,
                textvariable=self.port_var,
                state="normal",
            )
            self.port_combo.grid(row=0, column=1, sticky="ew", padx=(0, 6))
            ttk.Button(
                connection,
                text="重新掃描",
                command=self._refresh_ports,
            ).grid(row=0, column=2, padx=(0, 14))

            ttk.Label(connection, text="Baud").grid(
                row=0, column=3, sticky="w", padx=(0, 6)
            )
            ttk.Combobox(
                connection,
                textvariable=self.baud_var,
                values=BAUD_RATES,
                width=9,
            ).grid(row=0, column=4, padx=(0, 14))

            ttk.Label(connection, text="Timeout (秒)").grid(
                row=0, column=5, sticky="w", padx=(0, 6)
            )
            ttk.Entry(connection, textvariable=self.timeout_var, width=7).grid(
                row=0, column=6
            )

            commands = ttk.LabelFrame(self, text="快速指令", padding=10)
            commands.grid(row=1, column=0, sticky="ew", padx=12, pady=6)
            for column in range(6):
                commands.columnconfigure(column, weight=1)

            informational = ("PING", "HELP", "STATUS")
            flight_controls = (
                "ENABLE_STREAM",
                "START_MISSION",
                "START_OFFBOARD",
                "STOP_OFFBOARD",
                "LAND",
                "RTL",
                "ABORT",
            )
            for column, command in enumerate(informational):
                self._add_command_button(commands, command, 0, column)
            for index, command in enumerate(flight_controls):
                self._add_command_button(
                    commands,
                    command,
                    1 + index // 4,
                    index % 4,
                    dangerous=True,
                )

            goto = ttk.LabelFrame(self, text="GPS 目標", padding=10)
            goto.grid(row=2, column=0, sticky="ew", padx=12, pady=6)
            goto.columnconfigure(1, weight=1)
            goto.columnconfigure(3, weight=1)
            goto.columnconfigure(5, weight=1)

            ttk.Label(goto, text="緯度 Latitude").grid(row=0, column=0, sticky="w")
            ttk.Entry(goto, textvariable=self.latitude_var).grid(
                row=0, column=1, sticky="ew", padx=(6, 14)
            )
            ttk.Label(goto, text="經度 Longitude").grid(row=0, column=2, sticky="w")
            ttk.Entry(goto, textvariable=self.longitude_var).grid(
                row=0, column=3, sticky="ew", padx=(6, 14)
            )
            ttk.Label(goto, text="高度 (m)").grid(row=0, column=4, sticky="w")
            ttk.Entry(goto, textvariable=self.altitude_var).grid(
                row=0, column=5, sticky="ew", padx=(6, 0)
            )

            hint = (
                "GOTO：相對 Home 高度　｜　GOTO_AMSL：平均海平面高度。"
                "送出前請先用 STATUS 確認 ready_for_goto=true。"
            )
            ttk.Label(goto, text=hint, wraplength=850).grid(
                row=1, column=0, columnspan=6, sticky="w", pady=(8, 6)
            )

            goto_buttons = ttk.Frame(goto)
            goto_buttons.grid(row=2, column=0, columnspan=6, sticky="ew")
            goto_buttons.columnconfigure(0, weight=1)
            goto_buttons.columnconfigure(1, weight=1)
            self._add_command_button(
                goto_buttons,
                "GOTO",
                0,
                0,
                text="送出 GOTO（相對 Home）",
                dangerous=True,
            )
            self._add_command_button(
                goto_buttons,
                "GOTO_AMSL",
                0,
                1,
                text="送出 GOTO_AMSL（海拔）",
                dangerous=True,
            )

            event_frame = ttk.LabelFrame(self, text="通訊紀錄", padding=8)
            event_frame.grid(
                row=3, column=0, sticky="nsew", padx=12, pady=(6, 6)
            )
            event_frame.columnconfigure(0, weight=1)
            event_frame.rowconfigure(0, weight=1)
            self.log = scrolledtext.ScrolledText(
                event_frame,
                height=14,
                state="disabled",
                wrap="word",
                font=("TkFixedFont", 10),
            )
            self.log.grid(row=0, column=0, sticky="nsew")
            self.log.tag_configure("sent", foreground="#1d4ed8")
            self.log.tag_configure("ack", foreground="#15803d")
            self.log.tag_configure("error", foreground="#b91c1c")
            self.log.tag_configure("notice", foreground="#6b7280")

            bottom = ttk.Frame(self, padding=(12, 0, 12, 10))
            bottom.grid(row=4, column=0, sticky="ew")
            bottom.columnconfigure(0, weight=1)
            ttk.Label(
                bottom,
                textvariable=self.status_var,
                style="Status.TLabel",
            ).grid(row=0, column=0, sticky="w")
            ttk.Button(bottom, text="清除紀錄", command=self._clear_log).grid(
                row=0, column=1
            )

        def _add_command_button(
            self,
            parent: tk.Misc,
            command: str,
            row: int,
            column: int,
            *,
            text: str | None = None,
            dangerous: bool = False,
        ) -> None:
            style = "Danger.TButton" if dangerous else "Command.TButton"
            button = ttk.Button(
                parent,
                text=text or command,
                style=style,
                command=lambda selected=command: self._request_send(selected),
            )
            button.grid(row=row, column=column, sticky="ew", padx=4, pady=4)
            self._command_buttons.append(button)

        def _refresh_ports(self, *, log_result: bool = True) -> None:
            ports = available_serial_ports()
            current = self.port_var.get().strip()
            self.port_combo.configure(values=ports)
            if not current and ports:
                self.port_var.set(ports[0])
            if log_result:
                if ports:
                    self._append_log(
                        "找到 serial ports：" + ", ".join(ports), "notice"
                    )
                else:
                    self._append_log(
                        "未偵測到 serial port；可手動輸入 COM10 或 /dev/ttyUSB0。",
                        "notice",
                    )

        def _read_settings(self) -> tuple[str, int, float]:
            port = self.port_var.get().strip()
            if not port:
                raise ValueError("請先選擇或輸入 serial port")
            try:
                baud_rate = int(self.baud_var.get())
            except ValueError as exc:
                raise ValueError("Baud rate 必須是整數") from exc
            try:
                timeout = float(self.timeout_var.get())
            except ValueError as exc:
                raise ValueError("Timeout 必須是數字") from exc
            if baud_rate <= 0:
                raise ValueError("Baud rate 必須大於 0")
            if timeout <= 0.0 or not math.isfinite(timeout):
                raise ValueError("Timeout 必須是大於 0 的有限數值")
            return port, baud_rate, timeout

        def _arguments_for(self, command: str) -> tuple[str, ...]:
            if command not in {"GOTO", "GOTO_AMSL"}:
                return ()
            arguments = (
                self.latitude_var.get().strip(),
                self.longitude_var.get().strip(),
                self.altitude_var.get().strip(),
            )
            if not all(arguments):
                raise ValueError("GOTO 必須填寫緯度、經度與高度")
            return arguments

        def _request_send(self, command: str) -> None:
            if self._busy:
                return
            try:
                port, baud_rate, timeout = self._read_settings()
                arguments = self._arguments_for(command)
                # Validate before presenting the confirmation dialog or opening a port.
                build_frame("preview", command, *arguments)
            except ValueError as exc:
                messagebox.showerror("輸入錯誤", str(exc), parent=self)
                return

            if command in FLIGHT_COMMANDS:
                detail = f"即將透過 {port} 發送 {command}"
                if arguments:
                    detail += "\n\n參數：" + ", ".join(arguments)
                detail += (
                    "\n\n這會影響飛行狀態。請確認已有獨立 RC/ELRS 接管方式，"
                    "且目前狀態允許執行。是否繼續？"
                )
                if not messagebox.askyesno("確認飛航指令", detail, parent=self):
                    return

            self._set_busy(True, f"正在發送 {command}，等待回覆…")
            worker = threading.Thread(
                target=self._send_worker,
                args=(port, baud_rate, timeout, command, arguments),
                daemon=True,
            )
            worker.start()

        def _send_worker(
            self,
            port: str,
            baud_rate: int,
            timeout: float,
            command: str,
            arguments: tuple[str, ...],
        ) -> None:
            try:
                result = send_command(
                    port,
                    baud_rate,
                    timeout,
                    command,
                    arguments,
                    on_sent=lambda frame: self._events.put(("sent", frame)),
                )
            except Exception as exc:  # Report serial/OS failures in the GUI.
                self._events.put(("failure", exc))
                return
            self._events.put(("complete", result))

        def _process_events(self) -> None:
            try:
                while True:
                    event, payload = self._events.get_nowait()
                    if event == "sent":
                        self._append_log("> " + str(payload).strip(), "sent")
                    elif event == "failure":
                        self._append_log("錯誤：" + str(payload), "error")
                        self._set_busy(False, "發送失敗")
                        messagebox.showerror("發送失敗", str(payload), parent=self)
                    elif event == "complete":
                        result = payload
                        assert isinstance(result, SendResult)
                        if result.response is None:
                            self._append_log(
                                "逾時：期限內沒有收到相同 sequence 的 ACK/ERR。"
                                "飛航指令可能已執行，請勿直接重送。",
                                "error",
                            )
                            self._set_busy(False, "等待回覆逾時")
                        else:
                            response_text = format_response(result.response)
                            tag = (
                                "ack"
                                if result.response.frame_type == "ACK"
                                else "error"
                            )
                            self._append_log("< " + response_text, tag)
                            self._set_busy(
                                False,
                                f"{result.response.frame_type}: "
                                f"{result.response.message}",
                            )
            except queue.Empty:
                pass
            self.after(100, self._process_events)

        def _set_busy(self, busy: bool, status: str) -> None:
            self._busy = busy
            state = "disabled" if busy else "normal"
            for button in self._command_buttons:
                button.configure(state=state)
            self.status_var.set(status)

        def _append_log(self, message: str, tag: str) -> None:
            timestamp = time.strftime("%H:%M:%S")
            self.log.configure(state="normal")
            self.log.insert("end", f"[{timestamp}] {message}\n", tag)
            self.log.configure(state="disabled")
            self.log.see("end")

        def _clear_log(self) -> None:
            self.log.configure(state="normal")
            self.log.delete("1.0", "end")
            self.log.configure(state="disabled")


def main() -> int:
    if tk is None:
        print(
            "Tkinter is unavailable. Install the Tk package for your Python "
            "installation (for example: sudo apt install python3-tk)."
        )
        return 1
    if serial is None:
        messagebox.showerror(
            "缺少 pyserial",
            "請先執行 'py -m pip install pyserial' 或 "
            "'python3 -m pip install pyserial'。",
        )
        return 1

    app = LR24CommandApp()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
