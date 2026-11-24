#!/usr/bin/env python3
"""Find the ESP32 and start the drive link, then open the control page."""

from __future__ import annotations

import sys
import threading
import time
import urllib.error
import urllib.request
import webbrowser

from pc_drive import HTTP_PORT, main as run_link

PAGE = f"http://127.0.0.1:{HTTP_PORT}/"


def page_up() -> bool:
    try:
        with urllib.request.urlopen(PAGE, timeout=0.5) as resp:
            return resp.status == 200
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def open_when_ready() -> None:
    for _ in range(50):
        if page_up():
            webbrowser.open(PAGE)
            return
        time.sleep(0.1)


def main() -> int:
    if page_up():
        print(f"Link already running: {PAGE}")
        webbrowser.open(PAGE)
        return 0
    threading.Thread(target=open_when_ready, daemon=True).start()
    return run_link()


if __name__ == "__main__":
    sys.exit(main())
