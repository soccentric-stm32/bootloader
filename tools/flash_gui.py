#!/usr/bin/env python3
#
# flash_gui.py - PySide6 GUI to flash BOOT over SWD
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-04
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
#
# A small front-end for the commands that already exist in
# scripts/docker-dev.sh: flash, erase, reset and probe.  Everything runs in
# the stm32-dev container by default, so the host needs Docker but no
# arm-none-eabi-gcc or OpenOCD.  Turn the switch off to use the host's
# OpenOCD/st-info instead (scripts/install-tools.sh).
#
# Usage:
#   make gui                (installs PySide6 first: make gui-deps)
#   tools/flash_gui.py [-f build/boot.elf]
#
# Shortcuts: Ctrl+O browse, Ctrl+Enter flash, Ctrl+R reset, Esc cancel.
# Firmware can also be dragged onto the window.
#

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import (
    Property,
    QEasingCurve,
    QProcess,
    QPropertyAnimation,
    QRectF,
    QSettings,
    QSize,
    Qt,
)
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontDatabase,
    QKeySequence,
    QPainter,
    QShortcut,
)
from PySide6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QFileDialog,
    QFrame,
    QGraphicsDropShadowEffect,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QStyle,
    QVBoxLayout,
    QWidget,
)

ROOT = Path(__file__).resolve().parent.parent
DEV_SCRIPT = ROOT / "scripts" / "docker-dev.sh"
DEFAULT_ELF = ROOT / "build" / "boot.elf"
DEFAULT_ADDR = "0x08000000"
FIRMWARE_SUFFIXES = (".elf", ".hex", ".bin")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# macOS-style light theme: grouped white cards on the window background,
# system blue as the only accent.
STYLE = """
QMainWindow, QWidget#Root { background: #ececec; color: #1d1d1f; }
QLabel { background: transparent; }

QLabel#Title { font-size: 23px; font-weight: 700; color: #1d1d1f; }
QLabel#Subtitle { font-size: 12px; color: #86868b; }
QLabel#Badge { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                       stop:0 #5aa9ff, stop:1 #007aff);
               color: #ffffff; border-radius: 13px;
               font-size: 26px; font-weight: 700; }
QLabel#SectionTitle { font-size: 13px; font-weight: 600; color: #1d1d1f; }
QLabel#RowLabel { font-size: 13px; color: #1d1d1f; }
QLabel#Hint { font-size: 12px; color: #86868b; }

QLabel#StatusChip { border-radius: 10px; padding: 3px 11px;
                    font-size: 11px; font-weight: 600;
                    background: #e8e8ed; color: #6e6e73; }
QLabel#StatusChip[state="idle"]  { background: #e8e8ed; color: #6e6e73; }
QLabel#StatusChip[state="busy"]  { background: #e5f1ff; color: #0071e3; }
QLabel#StatusChip[state="ok"]    { background: #e3f9e5; color: #248a3d; }
QLabel#StatusChip[state="error"] { background: #ffe5e5; color: #d70015; }

QFrame#Card { background: #ffffff; border: 1px solid #e3e3e6;
              border-radius: 12px; }
QFrame#Separator { background: #e8e8ec; border: none; }

QLineEdit { background: #ffffff; border: 1px solid #d2d2d7; border-radius: 7px;
            padding: 7px 9px; font-size: 13px;
            selection-background-color: #b3d7ff; selection-color: #1d1d1f; }
QLineEdit:focus { border: 1px solid #007aff; }
QLineEdit:disabled { background: #f5f5f7; color: #a1a1a6; }

QPushButton { background: #ffffff; border: 1px solid #d2d2d7; border-radius: 8px;
              padding: 7px 16px; font-size: 13px; font-weight: 500;
              color: #1d1d1f; }
QPushButton:hover { background: #f5f5f7; }
QPushButton:pressed { background: #e8e8ed; }
QPushButton:disabled { background: #f5f5f7; color: #a1a1a6;
                       border-color: #e3e3e6; }

QPushButton#Primary { background: #007aff; border: 1px solid #0071e3;
                      color: #ffffff; font-size: 15px; font-weight: 600; }
QPushButton#Primary:hover { background: #0071e3; }
QPushButton#Primary:pressed { background: #0062cc; }
QPushButton#Primary:disabled { background: #b9d9ff; border-color: #b9d9ff;
                               color: #ffffff; }

QPushButton#Danger { color: #ff3b30; }
QPushButton#Danger:hover { background: #fff1f0; border-color: #ff3b30; }
QPushButton#Danger:disabled { color: #ffb3ae; }

QPushButton#Link { background: transparent; border: none; color: #007aff;
                   padding: 4px 6px; font-size: 13px; font-weight: 500; }
QPushButton#Link:hover { color: #0071e3; background: transparent; }

QPlainTextEdit#Log { background: #ffffff; border: 1px solid #d2d2d7;
                     border-radius: 9px; padding: 8px; color: #1d1d1f;
                     selection-background-color: #b3d7ff;
                     selection-color: #1d1d1f; }

QProgressBar { background: #dcdce1; border: none; border-radius: 2px; }
QProgressBar::chunk { background: #007aff; border-radius: 2px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #c7c7cc; border-radius: 5px;
                              min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #a1a1a6; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #c7c7cc; border-radius: 5px;
                                min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: #a1a1a6; }
"""


def human_size(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.1f} MB"
    if size >= 1024:
        return f"{size / 1024:.1f} KB"
    return f"{size} B"


def _pick_family(candidates: list[str], fallback: str) -> str:
    families = set(QFontDatabase.families())
    for name in candidates:
        if name in families:
            return name
    return fallback


def ui_font() -> QFont:
    """A macOS-like UI font, falling back to whatever the system has."""
    family = _pick_family(
        ["SF Pro Text", "Helvetica Neue", "Inter", "Segoe UI", "Noto Sans",
         "DejaVu Sans"], "Sans Serif")
    return QFont(family, 13)


def mono_font() -> QFont:
    family = _pick_family(
        ["SF Mono", "Menlo", "JetBrains Mono", "Cascadia Mono", "Consolas",
         "DejaVu Sans Mono"], "Monospace")
    return QFont(family, 12)


class Switch(QAbstractButton):
    """macOS-style animated toggle switch."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setCheckable(True)
        self.setCursor(Qt.PointingHandCursor)
        self.setFixedSize(40, 24)
        self._offset = 0.0
        self._anim = QPropertyAnimation(self, b"offset", self)
        self._anim.setDuration(140)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)
        self.toggled.connect(self._animate)

    def _animate(self, checked: bool) -> None:
        self._anim.stop()
        self._anim.setStartValue(self._offset)
        self._anim.setEndValue(1.0 if checked else 0.0)
        self._anim.start()

    def sizeHint(self) -> QSize:  # noqa: N802 (Qt API)
        return QSize(40, 24)

    def _get_offset(self) -> float:
        return self._offset

    def _set_offset(self, value: float) -> None:
        self._offset = value
        self.update()

    offset = Property(float, _get_offset, _set_offset)

    def paintEvent(self, event) -> None:  # noqa: N802 (Qt API)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.setPen(Qt.NoPen)

        width, height = self.width(), self.height()
        radius = height / 2.0
        off = QColor("#d1d1d6") if self.isEnabled() else QColor("#e5e5ea")
        on = QColor("#007aff") if self.isEnabled() else QColor("#b9d9ff")
        t = self._offset
        track = QColor(
            round(off.red() + (on.red() - off.red()) * t),
            round(off.green() + (on.green() - off.green()) * t),
            round(off.blue() + (on.blue() - off.blue()) * t),
        )
        painter.setBrush(track)
        painter.drawRoundedRect(QRectF(0, 0, width, height), radius, radius)

        knob = height - 4
        x = 2 + t * (width - knob - 4)
        painter.setBrush(QColor("#ffffff"))
        painter.drawEllipse(QRectF(x, 2, knob, knob))
        painter.end()


def add_shadow(widget: QWidget) -> None:
    """Subtle macOS-style card shadow."""
    effect = QGraphicsDropShadowEffect(widget)
    effect.setBlurRadius(18)
    effect.setOffset(0, 2)
    effect.setColor(QColor(0, 0, 0, 28))
    widget.setGraphicsEffect(effect)


class FlashWindow(QMainWindow):
    """Flash, erase, reset and probe a NUCLEO-F767ZI from one window."""

    def __init__(self, firmware: Path | None) -> None:
        super().__init__()
        self.setWindowTitle("BOOT Flasher")
        self.setMinimumSize(860, 640)
        self.resize(880, 680)
        self.setWindowIcon(self.style().standardIcon(QStyle.SP_ComputerIcon))
        self.setAcceptDrops(True)

        self.settings = QSettings("soccentric", "boot-flasher")

        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.MergedChannels)
        self.process.readyReadStandardOutput.connect(self._drain)
        self.process.finished.connect(self._on_finished)
        self.process.errorOccurred.connect(self._on_error)

        self._build_ui()
        self._load_settings(firmware)
        self._install_shortcuts()
        self._refresh_hint()
        self._set_state("idle", "Ready")

    # ---- UI -------------------------------------------------------------

    def _build_ui(self) -> None:
        central = QWidget(self)
        central.setObjectName("Root")
        root = QVBoxLayout(central)
        root.setContentsMargins(22, 18, 22, 14)
        root.setSpacing(14)

        root.addLayout(self._header())
        root.addWidget(self._firmware_card())
        root.addLayout(self._actions())
        root.addWidget(self._output_card(), 1)

        self.progress = QProgressBar()
        self.progress.setRange(0, 0)
        self.progress.setTextVisible(False)
        self.progress.setFixedHeight(4)
        self.progress.hide()
        root.addWidget(self.progress)

        footer = QHBoxLayout()
        self.status = QLabel("Ready")
        self.status.setObjectName("Hint")
        self.version = QLabel("Version 0.1")
        self.version.setObjectName("Hint")
        footer.addWidget(self.status, 1)
        footer.addWidget(self.version)
        root.addLayout(footer)

        self.setCentralWidget(central)
        self.setStyleSheet(STYLE)

    def _header(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(14)

        badge = QLabel("B")
        badge.setObjectName("Badge")
        badge.setAlignment(Qt.AlignCenter)
        badge.setFixedSize(52, 52)

        titles = QVBoxLayout()
        titles.setSpacing(0)
        title = QLabel("BOOT Flasher")
        title.setObjectName("Title")
        subtitle = QLabel("NUCLEO-F767ZI  ·  STM32F767ZIT6  ·  SWD / ST-LINK")
        subtitle.setObjectName("Subtitle")
        titles.addWidget(title)
        titles.addWidget(subtitle)

        self.status_chip = QLabel("Ready")
        self.status_chip.setObjectName("StatusChip")
        self.status_chip.setAlignment(Qt.AlignCenter)

        row.addWidget(badge)
        row.addLayout(titles)
        row.addStretch(1)
        row.addWidget(self.status_chip)
        return row

    @staticmethod
    def _separator() -> QFrame:
        line = QFrame()
        line.setObjectName("Separator")
        line.setFixedHeight(1)
        return line

    def _firmware_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("Card")
        box = QVBoxLayout(card)
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(0)

        # Firmware file row: label | path field + hint | Choose…
        label = QLabel("Firmware")
        label.setObjectName("RowLabel")
        label.setFixedWidth(160)
        label.setAlignment(Qt.AlignLeft | Qt.AlignTop)

        self.file_edit = QLineEdit()
        self.file_edit.setPlaceholderText(
            "Drag a .elf, .hex or .bin here, or choose a file")
        self.file_edit.setClearButtonEnabled(True)
        self.file_edit.textChanged.connect(self._refresh_hint)

        self.file_hint = QLabel("")
        self.file_hint.setObjectName("Hint")

        field = QVBoxLayout()
        field.setSpacing(3)
        field.addWidget(self.file_edit)
        field.addWidget(self.file_hint)

        choose = QPushButton("Choose…")
        choose.clicked.connect(self._browse)

        row = QHBoxLayout()
        row.setContentsMargins(16, 11, 16, 11)
        row.setSpacing(12)
        row.addWidget(label)
        row.addLayout(field, 1)
        row.addWidget(choose, 0, Qt.AlignTop)
        box.addLayout(row)
        box.addWidget(self._separator())

        # Load address row.
        label = QLabel("Load address")
        label.setObjectName("RowLabel")
        label.setFixedWidth(160)
        self.addr_edit = QLineEdit(DEFAULT_ADDR)
        self.addr_edit.setFixedWidth(120)
        self.addr_edit.setAlignment(Qt.AlignCenter)
        self.addr_edit.setToolTip("Load address, used for .bin files only")
        note = QLabel("for .bin images")
        note.setObjectName("Hint")

        row = QHBoxLayout()
        row.setContentsMargins(16, 11, 16, 11)
        row.setSpacing(12)
        row.addWidget(label)
        row.addWidget(self.addr_edit)
        row.addWidget(note)
        row.addStretch(1)
        box.addLayout(row)
        box.addWidget(self._separator())

        # Container row.
        label = QLabel("Use stm32-dev container")
        label.setObjectName("RowLabel")
        label.setFixedWidth(160)
        self.container_switch = Switch()
        self.container_switch.setToolTip(
            "Uses scripts/docker-dev.sh; turn off to use the host's OpenOCD")
        self.container_switch.setChecked(
            DEV_SCRIPT.is_file() and shutil.which("docker") is not None)

        row = QHBoxLayout()
        row.setContentsMargins(16, 11, 16, 11)
        row.setSpacing(12)
        row.addWidget(label)
        hint = QLabel("uses scripts/docker-dev.sh")
        hint.setObjectName("Hint")
        row.addWidget(hint)
        row.addStretch(1)
        row.addWidget(self.container_switch)
        box.addLayout(row)

        add_shadow(card)
        return card

    def _actions(self) -> QVBoxLayout:
        column = QVBoxLayout()
        column.setSpacing(8)

        row = QHBoxLayout()
        row.setSpacing(10)

        self.flash_button = QPushButton("Flash")
        self.flash_button.setObjectName("Primary")
        self.flash_button.setMinimumHeight(52)
        self.flash_button.setDefault(True)
        self.flash_button.setToolTip("Program, verify and reset (Ctrl+Enter)")
        self.flash_button.clicked.connect(self._flash)

        self.erase_button = QPushButton("Erase")
        self.erase_button.setObjectName("Danger")
        self.erase_button.setMinimumHeight(52)
        self.erase_button.setToolTip("Erase the whole flash")
        self.erase_button.clicked.connect(self._erase)

        self.reset_button = QPushButton("Reset")
        self.reset_button.setMinimumHeight(52)
        self.reset_button.setToolTip("Reset the target and let it run (Ctrl+R)")
        self.reset_button.clicked.connect(self._reset)

        self.probe_button = QPushButton("Probe")
        self.probe_button.setMinimumHeight(52)
        self.probe_button.setToolTip("Detect the ST-LINK and the target chip")
        self.probe_button.clicked.connect(self._probe)

        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setMinimumHeight(52)
        self.cancel_button.setEnabled(False)
        self.cancel_button.setToolTip("Stop the running command (Esc)")
        self.cancel_button.clicked.connect(self.process.kill)

        for button in (self.flash_button, self.erase_button,
                       self.reset_button, self.probe_button):
            row.addWidget(button, 1)
        row.addWidget(self.cancel_button, 1)
        column.addLayout(row)
        return column

    def _output_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("Card")
        box = QVBoxLayout(card)
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(0)

        header = QHBoxLayout()
        header.setContentsMargins(16, 11, 16, 6)
        label = QLabel("Output")
        label.setObjectName("SectionTitle")
        clear = QPushButton("Clear")
        clear.setObjectName("Link")
        clear.setCursor(Qt.PointingHandCursor)
        header.addWidget(label)
        header.addStretch(1)
        header.addWidget(clear)
        box.addLayout(header)

        self.log = QPlainTextEdit()
        self.log.setObjectName("Log")
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        self.log.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.log.setFont(mono_font())
        self.log.setPlaceholderText("OpenOCD output will appear here.")
        clear.clicked.connect(self.log.clear)

        body = QHBoxLayout()
        body.setContentsMargins(16, 0, 16, 16)
        body.addWidget(self.log)
        box.addLayout(body, 1)

        add_shadow(card)
        return card

    def _install_shortcuts(self) -> None:
        QShortcut(QKeySequence("Ctrl+O"), self, self._browse)
        QShortcut(QKeySequence("Ctrl+Return"), self, self._flash)
        QShortcut(QKeySequence("Ctrl+Enter"), self, self._flash)
        QShortcut(QKeySequence("Ctrl+R"), self, self._reset)
        QShortcut(QKeySequence("Esc"), self, self._cancel)

    # ---- state / settings ----------------------------------------------

    def _load_settings(self, firmware: Path | None) -> None:
        saved = self.settings.value("firmware", "", type=str)
        if firmware is not None:
            self.file_edit.setText(self._display_path(firmware))
        elif saved and self._resolve(saved).is_file():
            self.file_edit.setText(saved)
        elif DEFAULT_ELF.is_file():
            self.file_edit.setText(self._display_path(DEFAULT_ELF))

        self.container_switch.setChecked(
            self.settings.value("docker", self.container_switch.isChecked(),
                                type=bool))
        self.addr_edit.setText(
            self.settings.value("address", DEFAULT_ADDR, type=str))

    def _save_settings(self) -> None:
        self.settings.setValue("firmware", self.file_edit.text().strip())
        self.settings.setValue("docker", self.container_switch.isChecked())
        self.settings.setValue("address", self.addr_edit.text().strip())

    def closeEvent(self, event) -> None:  # noqa: N802 (Qt API)
        self._save_settings()
        if self.process.state() != QProcess.NotRunning:
            self.process.kill()
            self.process.waitForFinished(1000)
        super().closeEvent(event)

    def _set_state(self, state: str, text: str) -> None:
        self.status_chip.setProperty("state", state)
        self.status_chip.setText(text)
        self.status_chip.style().unpolish(self.status_chip)
        self.status_chip.style().polish(self.status_chip)

    def _set_busy(self, busy: bool, text: str) -> None:
        for widget in (self.flash_button, self.erase_button,
                       self.reset_button, self.probe_button,
                       self.file_edit, self.addr_edit,
                       self.container_switch):
            widget.setEnabled(not busy)
        self.cancel_button.setEnabled(busy)
        self.progress.setVisible(busy)
        self.status.setText(text)
        if busy:
            self._set_state("busy", "Working…")

    def _refresh_hint(self) -> None:
        if not self.file_edit.text().strip():
            self.file_hint.setText("No firmware selected")
            return
        path = self._resolve(self.file_edit.text())
        if not path.is_file():
            self.file_hint.setText("File not found")
            return
        self.file_edit.setToolTip(str(path))
        try:
            stat = path.stat()
            stamp = datetime.fromtimestamp(stat.st_mtime).strftime("%H:%M:%S")
            self.file_hint.setText(f"{path.name}  ·  {human_size(stat.st_size)}"
                                   f"  ·  {stamp}")
        except OSError:
            self.file_hint.setText(path.name)

    @staticmethod
    def _resolve(text: str) -> Path:
        """Resolve a path typed in the UI; relative paths are project-relative."""
        path = Path(text.strip()).expanduser()
        return path if path.is_absolute() else ROOT / path

    @staticmethod
    def _display_path(path: Path) -> str:
        """Show paths inside the project relative to the repo root."""
        try:
            return path.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            return str(path)

    # ---- process helpers ------------------------------------------------

    def _append(self, text: str) -> None:
        text = ANSI_RE.sub("", text).rstrip("\r\n")
        if text:
            self.log.appendPlainText(text)

    def _drain(self) -> None:
        # MergedChannels forwards stdout and stderr to the stdout channel.
        buf = self.process.readAllStandardOutput()
        self._append(bytes(buf).decode("utf-8", "replace"))

    def _start(self, program: str, args: list[str], text: str) -> None:
        if self.process.state() != QProcess.NotRunning:
            return
        self.log.appendPlainText("$ " + " ".join([program, *args]))
        self._set_busy(True, text)
        self.process.start(program, args)

    def _project_path(self, path: Path) -> str:
        """Return the host path, which docker-dev.sh maps into /work itself."""
        path = path.resolve()
        try:
            path.relative_to(ROOT)
        except ValueError as exc:
            raise RuntimeError(
                f"{path} is outside {ROOT}; the container only sees /work"
            ) from exc
        return str(path)

    def _openocd_cfg(self) -> str:
        return os.environ.get("OPENOCD_CFG", "board/st_nucleo_f7.cfg")

    def _dev(self, *args: str) -> None:
        self._start("bash", [str(DEV_SCRIPT), *args], "Running…")

    # ---- actions --------------------------------------------------------

    def _browse(self) -> None:
        text = self.file_edit.text().strip()
        start = self._resolve(text) if text else None
        start_dir = start.parent if start and start.parent.is_dir() else (
            DEFAULT_ELF.parent if DEFAULT_ELF.parent.is_dir() else ROOT)
        name, _ = QFileDialog.getOpenFileName(
            self, "Choose firmware", str(start_dir),
            "Firmware (*.elf *.hex *.bin);;All files (*)")
        if name:
            self.file_edit.setText(self._display_path(Path(name)))

    def _firmware(self) -> Path | None:
        if not self.file_edit.text().strip():
            path = None
        else:
            path = self._resolve(self.file_edit.text())
        if path is None or not path.is_file():
            QMessageBox.warning(self, "BOOT Flasher",
                                "Choose an existing firmware file first.")
            return None
        if path.suffix.lower() not in FIRMWARE_SUFFIXES:
            QMessageBox.warning(self, "BOOT Flasher",
                                "Firmware must be .elf, .hex or .bin.")
            return None
        return path

    def _flash(self) -> None:
        firmware = self._firmware()
        if firmware is None:
            return
        suffix = firmware.suffix.lower()
        addr = self.addr_edit.text().strip() or DEFAULT_ADDR

        try:
            if self.container_switch.isChecked():
                args = ["flash", self._project_path(firmware)]
                if suffix == ".bin":
                    args.append(addr)
                self._dev(*args)
            else:
                if shutil.which("openocd") is None:
                    raise RuntimeError("openocd not found in PATH "
                                       "(see scripts/install-tools.sh)")
                prog_args = ["-f", self._openocd_cfg(), "-c"]
                target = f'"{firmware}"'
                if suffix == ".bin":
                    target += f" {addr}"
                self._start("openocd", prog_args + [
                    f"program {target} verify reset exit"], "Flashing…")
        except RuntimeError as exc:
            QMessageBox.critical(self, "BOOT Flasher", str(exc))

    def _erase(self) -> None:
        if QMessageBox.question(
                self, "Erase flash",
                "Erase the whole flash? BOOT and both application slots "
                "will be lost.") != QMessageBox.Yes:
            return
        if self.container_switch.isChecked():
            self._dev("erase")
        else:
            self._start("openocd", ["-f", self._openocd_cfg(), "-c",
                                    "init; reset halt; flash erase_sector 0 0 last; exit"],
                        "Erasing…")

    def _reset(self) -> None:
        if self.container_switch.isChecked():
            self._dev("reset")
        else:
            self._start("openocd", ["-f", self._openocd_cfg(), "-c",
                                    "init; reset run; exit"], "Resetting…")

    def _probe(self) -> None:
        if self.container_switch.isChecked():
            self._dev("probe")
        elif shutil.which("st-info") is None:
            QMessageBox.critical(self, "BOOT Flasher",
                                 "st-info not found in PATH "
                                 "(see scripts/install-tools.sh)")
        else:
            self._start("st-info", ["--probe"], "Probing…")

    def _cancel(self) -> None:
        if self.process.state() != QProcess.NotRunning:
            self.status.setText("Cancelling…")
            self.process.kill()

    # ---- drag and drop ---------------------------------------------------

    def dragEnterEvent(self, event) -> None:  # noqa: N802 (Qt API)
        if self._dropped_firmware(event) is not None:
            event.acceptProposedAction()

    def dropEvent(self, event) -> None:  # noqa: N802 (Qt API)
        path = self._dropped_firmware(event)
        if path is not None:
            self.file_edit.setText(self._display_path(path))
            event.acceptProposedAction()

    @staticmethod
    def _dropped_firmware(event) -> Path | None:
        urls = event.mimeData().urls()
        if not urls:
            return None
        path = Path(urls[0].toLocalFile())
        if path.is_file() and path.suffix.lower() in FIRMWARE_SUFFIXES:
            return path
        return None

    # ---- process callbacks ----------------------------------------------

    def _on_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        self._drain()
        if status == QProcess.NormalExit and code == 0:
            self._set_busy(False, "Done")
            self._set_state("ok", "Done")
        else:
            self._set_busy(False, f"Failed (exit code {code})")
            self._set_state("error", "Failed")

    def _on_error(self, error: QProcess.ProcessError) -> None:
        if error == QProcess.FailedToStart:
            self._set_busy(False, "Failed to start the command")
            self._set_state("error", "Failed")
            self._append("error: could not start the process")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Flash BOOT over SWD")
    parser.add_argument("-f", "--firmware", type=Path,
                        default=DEFAULT_ELF if DEFAULT_ELF.is_file() else None,
                        help="firmware to flash (default: build/boot.elf)")
    args = parser.parse_args(argv)

    app = QApplication(sys.argv[:1])
    app.setStyle("Fusion")
    app.setApplicationName("BOOT Flasher")
    app.setFont(ui_font())
    window = FlashWindow(args.firmware)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
