#!/usr/bin/env python3
#
# flash_gui.py - BOOT Flasher: PySide6 GUI to flash the bootloader and the
#                five firmware projects of the workspace
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-06
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
#
# Pages:
#   Bootloader    build BOOT and program it over SWD through OpenOCD, in the
#                 stm32-dev container or with the host tools.  Probe, reset,
#                 provision and erase are under More.
#   Applications  app-c-freertos, app-c-zephyr, app-rust-freertos,
#                 app-rust-zephyr and validation-firmware: clone the
#                 repository if it is missing, build it, sign its image with
#                 mkimage.py --sign (BSAFE-006) and upload the signed image
#                 through BOOT with bootctl.py.
#
# The sidebar's Board panel reads BOOT's version, slots, trusted key and
# security floor, which the application pages check before flashing.
#
# Usage:
#   make gui                (installs dependencies first: make gui-deps)
#   tools/flash_gui.py [-f build/boot.elf]
#
# Code lives in tools/gui/: window.py (main window), pages.py, widgets.py,
# theme.py, icons.py, projects.py (the project list), runner.py, device.py.
#

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PySide6.QtWidgets import QApplication

try:
    from gui import theme
    from gui.window import APP_NAME, MainWindow
except ImportError:  # pragma: no cover - running from an unusual CWD
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gui import theme
    from gui.window import APP_NAME, MainWindow


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Flash the bootloader and the firmware projects")
    parser.add_argument("-f", "--firmware", type=Path, default=None,
                        help="BOOT firmware to preselect (.elf, .hex, .bin)")
    args = parser.parse_args(argv)

    app = QApplication(sys.argv[:1])
    app.setStyle("Fusion")
    app.setApplicationName(APP_NAME)
    app.setOrganizationName("soccentric")
    app.setFont(theme.ui_font())
    window = MainWindow(str(args.firmware.resolve()) if args.firmware
                        else None)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
