#!/usr/bin/env python3
"""Read the Bluetooth pad on this PC and send wheel commands over USB serial."""

from __future__ import annotations

import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import serial
from serial.tools import list_ports
from winrt.system import Array, Double
from winrt.windows.gaming.input import GameControllerSwitchPosition, RawGameController

BAUD = 115200
SEND_HZ = 25
DEADZONE = 0.18
HTTP_PORT = 8765
HTML = (Path(__file__).with_name("motor.html")).read_bytes()

web_cmd = [0, 0, 0, 0]
GEAR_LETTERS = "PRND"
web_ms = 0.0
web_lock = threading.Lock()


def candidate_ports() -> list[str]:
    found = []
    for p in list_ports.comports():
        blob = f"{p.description} {p.manufacturer} {p.hwid}".upper()
        if "CH340" in blob or "CP210" in blob or "USB-SERIAL" in blob or "SILICON LABS" in blob:
            found.append(p.device)
    if not found:
        found = [p.device for p in list_ports.comports()]
    return found


def find_port() -> str:
    ports = candidate_ports()
    if not ports:
        raise SystemExit("No ESP32 serial port found. Plug in the USB cable.")
    return ports[0]


def open_serial(port: str | None = None) -> serial.Serial:
    ports = [port] if port else candidate_ports()
    last_error = None
    fallback = None
    for name in ports:
        ser = serial.Serial()
        ser.port = name
        ser.baudrate = BAUD
        ser.timeout = 0.05
        ser.write_timeout = 0.05
        try:
            ser.open()
        except serial.SerialException as exc:
            last_error = exc
            continue
        ser.dtr = False
        ser.rts = False
        ser.reset_input_buffer()
        deadline = time.time() + 1.5
        ready = False
        while time.time() < deadline:
            line = ser.readline().decode("ascii", "ignore").strip()
            if "USB READY" in line:
                ready = True
                break
        if ready:
            if fallback is not None:
                fallback.close()
            print(f"usb {name}")
            return ser
        if fallback is None:
            fallback = ser
        else:
            ser.close()
    if fallback is not None:
        print(f"usb {fallback.port} (no hello)")
        return fallback
    raise SystemExit(f"No ESP32 on USB. Last error: {last_error}")


def clamp(n: int, lo: int, hi: int) -> int:
    return lo if n < lo else hi if n > hi else n


def axis_to_stick(value: float) -> int:
    x = (value - 0.5) * 2.0
    if abs(x) < DEADZONE:
        return 0
    span = 1.0 - DEADZONE
    scaled = (x - DEADZONE) / span if x > 0 else (x + DEADZONE) / span
    return clamp(int(round(scaled * 100)), -100, 100)


def axis_to_trigger(value: float) -> int:
    if value < 0.04:
        return 0
    return clamp(int(round(value * 100)), 0, 100)


class Pad:
    def __init__(self) -> None:
        self.ctrl = None
        self.buttons = None
        self.switches = None
        self.axes = None
        self._from_event = []
        self._rescan = 0.0
        self._on_added = self._handle_added
        self._on_removed = self._handle_removed
        RawGameController.add_raw_game_controller_added(self._on_added)
        RawGameController.add_raw_game_controller_removed(self._on_removed)

    def _handle_added(self, _sender, ctrl) -> None:
        self._from_event.append(ctrl)

    def _handle_removed(self, _sender, ctrl) -> None:
        if self.ctrl is ctrl:
            self.ctrl = None
        self._from_event = [p for p in self._from_event if p is not ctrl]

    def attach(self) -> bool:
        pads = list(RawGameController.raw_game_controllers) or list(self._from_event)
        if not pads:
            self.ctrl = None
            return False
        ctrl = pads[0]
        if ctrl is self.ctrl:
            return True
        self.ctrl = ctrl
        self.buttons = Array(bool, ctrl.button_count)
        self.switches = Array(GameControllerSwitchPosition, ctrl.switch_count)
        self.axes = Array(Double, ctrl.axis_count)
        print(f"pad {ctrl.display_name}  axes {ctrl.axis_count}  buttons {ctrl.button_count}")
        return True

    def read(self) -> tuple[int, int, int, int]:
        if self.ctrl is None and not self.attach():
            return 0, 0, 0, 0
        try:
            ts = self.ctrl.get_current_reading(self.buttons, self.switches, self.axes)
        except Exception:
            self.ctrl = None
            return 0, 0, 0, 0
        if not ts:
            self.ctrl = None
            return 0, 0, 0, 0
        ax0 = self.axes[0] if self.ctrl.axis_count > 0 else 0.5
        ax1 = self.axes[1] if self.ctrl.axis_count > 1 else 0.5
        ax2 = self.axes[2] if self.ctrl.axis_count > 2 else 0.5
        sleeping = ax0 < 0.08 and ax1 < 0.08 and ax2 < 0.08
        if sleeping:
            now = time.time()
            if now - self._rescan > 1.0:
                self._rescan = now
                self.ctrl = None
            return 0, 0, 0, 0
        x = axis_to_stick(ax0)
        if self.ctrl.switch_count > 0:
            dpad = self.switches[0]
            if dpad == GameControllerSwitchPosition.LEFT:
                x = -100
            elif dpad == GameControllerSwitchPosition.RIGHT:
                x = 100
        cam = axis_to_stick(ax2)
        brake = axis_to_trigger(self.axes[3] if self.ctrl.axis_count > 3 else 0.0)
        gas = axis_to_trigger(self.axes[4] if self.ctrl.axis_count > 4 else 0.0)
        return x, gas, brake, cam


def send(ser: serial.Serial, x: int, g: int, b: int, c: int = 0, gear: int = 0) -> None:
    ser.write(f"{x} {g} {b} {c} {gear}\n".encode("ascii"))
    ser.flush()


class MotorHandler(BaseHTTPRequestHandler):
    def log_message(self, _fmt: str, *_args) -> None:
        return

    def do_GET(self) -> None:
        path = urlparse(self.path)
        if path.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(HTML)
            return
        if path.path == "/joy":
            q = parse_qs(path.query)
            global web_ms
            with web_lock:
                if "x" in q:
                    web_cmd[0] = clamp(int(q["x"][0]), -100, 100)
                if "g" in q:
                    web_cmd[1] = clamp(int(q["g"][0]), 0, 100)
                if "b" in q:
                    web_cmd[2] = clamp(int(q["b"][0]), 0, 100)
                if "gearset" in q:
                    web_cmd[3] = clamp(int(q["gearset"][0]), 0, 3)
                web_ms = time.time()
            self.send_response(204)
            self.end_headers()
            return
        self.send_error(404)


def start_page() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), MotorHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"page http://127.0.0.1:{HTTP_PORT}/")


def mix(pad, web: tuple[int, int, int, int]) -> tuple[int, int, int, int, int]:
    vals = list(pad) + [0, 0, 0, 0]
    px, pg, pb, pc = int(vals[0]), int(vals[1]), int(vals[2]), int(vals[3])
    gear = int(web[3])
    if abs(px) >= 6 or pg or pb or abs(pc) >= 6:
        return px, pg, pb, pc, gear
    return web[0], web[1], web[2], 0, gear


def main() -> int:
    start_page()
    ser = open_serial()
    pad = Pad()
    pad.attach()
    period = 1.0 / SEND_HZ
    last_print = 0.0
    last = None
    try:
        while True:
            t0 = time.perf_counter()
            with web_lock:
                if time.time() - web_ms > 0.2:
                    web = (0, 0, 0, web_cmd[3])
                else:
                    web = (web_cmd[0], web_cmd[1], web_cmd[2], web_cmd[3])
            x, g, b, c, gear = mix(pad.read(), web)
            try:
                send(ser, x, g, b, c, gear)
            except serial.SerialException:
                print("usb lost, reconnecting")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(1)
                ser = open_serial()
                last = None
                continue
            now = time.time()
            if (x, g, b, c, gear) != last or now - last_print > 1:
                last = (x, g, b, c, gear)
                last_print = now
                print(f"x {x:4d}  gas {g:3d}  brake {b:3d}  cam {c:4d}  gear {GEAR_LETTERS[gear]}")
            dt = time.perf_counter() - t0
            if dt < period:
                time.sleep(period - dt)
    except KeyboardInterrupt:
        try:
            send(ser, 0, 0, 0, 0, 0)
        except Exception:
            pass
        print("stop")
        return 0


if __name__ == "__main__":
    sys.exit(main())
