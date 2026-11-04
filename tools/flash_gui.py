#!/usr/bin/env python3
#
# flash_gui.py - PySide6 GUI to flash BOOT and application images
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-04
# Target:  NUCLEO-F767ZI (STM32F767ZI, on-board ST-LINK/V2.1)
#
# Three modes:
#   Bootloader   program BOOT (.elf/.hex/.bin) over SWD through OpenOCD,
#                either in the stm32-dev container or with the host tools.
#   Application  update an application image (.img) over the ST-LINK VCP
#                with the HWLINK protocol (tools/bootctl.py).  The target
#                slot is read-only and comes from BOOT's BootInfo.
#   Production   ordered build/provision/flash A+B/verify workflow.  The
#                device model and QProcess queue live in tools/gui/.
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
import glob
import json
import os
import re
import shutil
import struct
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
    QTimer,
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
    QButtonGroup,
    QComboBox,
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

try:
    from gui.device import parse_boot_info, stlink_ports
    from gui.runner import Job, Runner
except ImportError:  # pragma: no cover - running from an unusual CWD
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gui.device import parse_boot_info, stlink_ports
    from gui.runner import Job, Runner

ROOT = Path(__file__).resolve().parent.parent
DEV_SCRIPT = ROOT / "scripts" / "docker-dev.sh"
BOOTCTL = ROOT / "tools" / "bootctl.py"
DEFAULT_ELF = ROOT / "build" / "boot.elf"
DEFAULT_APP_IMG = ROOT / "build" / "app.img"
DEFAULT_ADDR = "0x08000000"
BOOT_SUFFIXES = (".elf", ".hex", ".bin")
APP_SUFFIXES = (".img",)
IMAGE_MAGIC = 0x48574249
IMAGE_PREFIX = struct.Struct("<IHHIII")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
PERCENT_RE = re.compile(r"(\d{1,3}(?:\.\d+)?)%")
WARN_COLOR = "#b25000"

# macOS-style light theme: grouped white cards on the window background,
# system blue as the only accent.
STYLE = """
QMainWindow, QWidget#Root { background: #f5f5f7; color: #1d1d1f; }
QWidget#Content { background: #f5f5f7; }
QWidget#Sidebar { background: #e9e9ec; border-right: 1px solid #dcdce1; }
QLabel { background: transparent; }

QLabel#SidebarTitle { font-size: 15px; font-weight: 700; color: #1d1d1f; }
QLabel#SidebarSubtitle { font-size: 11px; color: #86868b; }
QLabel#SidebarSection { font-size: 10px; font-weight: 700; color: #86868b; }
QPushButton#NavItem { background: transparent; border: 1px solid transparent;
                      border-radius: 7px; padding: 8px 10px; text-align: left;
                      font-size: 13px; font-weight: 500; color: #1d1d1f; }
QPushButton#NavItem:hover { background: rgba(0, 0, 0, 0.06); }
QPushButton#NavItem:pressed { background: rgba(0, 0, 0, 0.12); }
QPushButton#NavItem:checked { background: #007aff; color: #ffffff;
                              font-weight: 600; }

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

QFrame#SegmentBox { background: #f5f5f7; border: 1px solid #d2d2d7;
                    border-radius: 9px; }
QPushButton#Segment { background: transparent; border: 1px solid transparent;
                      border-radius: 7px; padding: 6px 20px; color: #1d1d1f;
                      font-size: 13px; font-weight: 500; }
QPushButton#Segment:hover { background: #e8e8ed; }
QPushButton#Segment:checked { background: #ffffff; border-color: #d2d2d7;
                              font-weight: 600; }
QPushButton#Segment:disabled { color: #c7c7cc; }

QLineEdit { background: #ffffff; border: 1px solid #d2d2d7; border-radius: 7px;
            padding: 7px 9px; font-size: 13px;
            selection-background-color: #b3d7ff; selection-color: #1d1d1f; }
QLineEdit:focus { border: 1px solid #007aff; }
QLineEdit:disabled { background: #f5f5f7; color: #a1a1a6; }

QComboBox { background: #ffffff; border: 1px solid #d2d2d7; border-radius: 7px;
            padding: 6px 9px; font-size: 13px; color: #1d1d1f; }
QComboBox:focus { border: 1px solid #007aff; }
QComboBox:disabled { background: #f5f5f7; color: #a1a1a6; }
QComboBox QAbstractItemView { background: #ffffff; border: 1px solid #d2d2d7;
                              selection-background-color: #e5f1ff;
                              selection-color: #1d1d1f; outline: none; }

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


def version_text(value: int) -> str:
    return f"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{value & 0xFF}"


def image_summary(path: Path) -> str | None:
    """'v1.2.3 · 640.0 KB' from a BOOT image header, or None."""
    try:
        with path.open("rb") as handle:
            head = handle.read(IMAGE_PREFIX.size)
    except OSError:
        return None
    if len(head) < IMAGE_PREFIX.size:
        return None
    magic, _hv, _hs, _type, version, size = IMAGE_PREFIX.unpack(head)
    if magic != IMAGE_MAGIC:
        return None
    return f"v{version_text(version)} · {human_size(size)}"


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
    """Flash BOOT over SWD and application images over HWLINK."""

    def __init__(self, firmware: Path | None) -> None:
        super().__init__()
        self.setWindowTitle("BOOT Flasher")
        self.setMinimumSize(980, 720)
        self.resize(1010, 760)
        self.setWindowIcon(self.style().standardIcon(QStyle.SP_ComputerIcon))
        self.setAcceptDrops(True)

        self.settings = QSettings("soccentric", "boot-flasher")
        self.mode = "boot"
        self.firmware_text = {"boot": "", "app": ""}
        self.active_slot: str | None = None
        self.target_slot: str | None = None
        self.flash_mode: str | None = None
        self.boot_version: str | None = None
        self.production_steps: dict[str, QLabel] = {}
        self.current_step: str | None = None

        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.MergedChannels)
        self.process.readyReadStandardOutput.connect(self._drain)
        self.process.finished.connect(self._on_finished)
        self.process.errorOccurred.connect(self._on_error)

        self.info_process = QProcess(self)
        self.info_process.finished.connect(self._on_info_finished)
        self.info_process.errorOccurred.connect(self._on_info_error)

        self.runner = Runner(self)
        self.runner.output.connect(self._append)
        self.runner.started.connect(self._on_runner_started)
        self.runner.finished.connect(self._on_runner_finished)
        self.runner.allFinished.connect(self._on_runner_all_finished)

        self.port_timer = QTimer(self)
        self.port_timer.setSingleShot(True)
        self.port_timer.setInterval(500)
        self.port_timer.timeout.connect(self._refresh_boot_info)

        self._build_ui()
        self._load_settings(firmware)
        self._install_shortcuts()
        self._set_mode("boot", initial=True)
        self._set_state("idle", "Ready")

    # ---- UI -------------------------------------------------------------

    def _build_ui(self) -> None:
        central = QWidget(self)
        central.setObjectName("Root")
        layout = QHBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self._sidebar())

        content = QWidget()
        content.setObjectName("Content")
        root = QVBoxLayout(content)
        root.setContentsMargins(24, 18, 24, 14)
        root.setSpacing(12)

        root.addLayout(self._header())
        self.firmware_card = self._firmware_card()
        root.addWidget(self.firmware_card)
        self.production_card = self._production_card()
        self.production_card.hide()
        root.addWidget(self.production_card)
        self.actions_widget = QWidget()
        self.actions_widget.setLayout(self._actions())
        root.addWidget(self.actions_widget)
        root.addWidget(self._output_card(), 1)

        self.progress = QProgressBar()
        self.progress.setRange(0, 0)
        self.progress.setTextVisible(False)
        self.progress.setFixedHeight(4)
        self.progress.hide()
        root.addWidget(self.progress)

        self.status = QLabel("Ready")
        self.status.setObjectName("Hint")
        root.addWidget(self.status)

        layout.addWidget(content, 1)
        self.setCentralWidget(central)
        self.setStyleSheet(STYLE)

    def _sidebar(self) -> QWidget:
        side = QWidget()
        side.setObjectName("Sidebar")
        side.setFixedWidth(216)
        column = QVBoxLayout(side)
        column.setContentsMargins(14, 18, 14, 14)
        column.setSpacing(4)

        top = QHBoxLayout()
        top.setSpacing(10)
        badge = QLabel("B")
        badge.setObjectName("Badge")
        badge.setAlignment(Qt.AlignCenter)
        badge.setFixedSize(44, 44)

        titles = QVBoxLayout()
        titles.setSpacing(0)
        title = QLabel("BOOT Flasher")
        title.setObjectName("SidebarTitle")
        subtitle = QLabel("NUCLEO-F767ZI")
        subtitle.setObjectName("SidebarSubtitle")
        titles.addWidget(title)
        titles.addWidget(subtitle)

        top.addWidget(badge)
        top.addLayout(titles)
        top.addStretch(1)
        column.addLayout(top)
        column.addSpacing(18)

        section = QLabel("FLASH")
        section.setObjectName("SidebarSection")
        column.addWidget(section)
        column.addSpacing(4)

        self.mode_group = QButtonGroup(self)
        self.mode_group.setExclusive(True)
        self.mode_boot = QPushButton("Bootloader")
        self.mode_app = QPushButton("Application")
        self.mode_prod = QPushButton("Production")
        for index, button in enumerate((self.mode_boot, self.mode_app,
                                        self.mode_prod)):
            button.setObjectName("NavItem")
            button.setCheckable(True)
            button.setCursor(Qt.PointingHandCursor)
            button.setMinimumHeight(34)
            self.mode_group.addButton(button, index)
            column.addWidget(button)
        self.mode_boot.setChecked(True)
        self.mode_group.idClicked.connect(
            lambda index: self._set_mode(("boot", "app", "prod")[index]))

        column.addStretch(1)
        version = QLabel("Version 0.1")
        version.setObjectName("Hint")
        column.addWidget(version)
        return side

    def _header(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(12)

        titles = QVBoxLayout()
        titles.setSpacing(0)
        self.page_title = QLabel("Bootloader")
        self.page_title.setObjectName("Title")
        self.page_subtitle = QLabel(
            "Program BOOT over SWD with OpenOCD")
        self.page_subtitle.setObjectName("Subtitle")
        titles.addWidget(self.page_title)
        titles.addWidget(self.page_subtitle)

        self.status_chip = QLabel("Ready")
        self.status_chip.setObjectName("StatusChip")
        self.status_chip.setAlignment(Qt.AlignCenter)

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

    def _card_row(self, widgets: list[QWidget],
                  stretch_index: int = -1) -> QWidget:
        """One grouped-card row with a hairline separator on top."""
        content = QWidget()
        layout = QHBoxLayout(content)
        layout.setContentsMargins(16, 10, 16, 10)
        layout.setSpacing(12)
        for index, widget in enumerate(widgets):
            if widget is None:
                layout.addStretch(1)
            elif index == stretch_index:
                layout.addWidget(widget, 1)
            else:
                layout.addWidget(widget)

        wrapper = QWidget()
        column = QVBoxLayout(wrapper)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(0)
        column.addWidget(self._separator())
        column.addWidget(content)
        return wrapper

    @staticmethod
    def _row_label(text: str) -> QLabel:
        label = QLabel(text)
        label.setObjectName("RowLabel")
        label.setFixedWidth(160)
        return label

    def _firmware_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("Card")
        box = QVBoxLayout(card)
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(0)

        # Always visible: firmware file.
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
        field_widget = QWidget()
        field_widget.setLayout(field)
        self.choose_button = QPushButton("Choose…")
        self.choose_button.clicked.connect(self._browse)

        content = QWidget()
        layout = QHBoxLayout(content)
        layout.setContentsMargins(16, 10, 16, 10)
        layout.setSpacing(12)
        layout.addWidget(self._row_label("Firmware"))
        layout.addWidget(field_widget, 1)
        layout.addWidget(self.choose_button, 0, Qt.AlignTop)
        box.addWidget(content)

        # BOOT mode: load address.
        self.addr_edit = QLineEdit(DEFAULT_ADDR)
        self.addr_edit.setFixedWidth(120)
        self.addr_edit.setAlignment(Qt.AlignCenter)
        self.addr_edit.setToolTip("Load address, used for .bin files only")
        note = QLabel("for .bin images")
        note.setObjectName("Hint")
        self.address_row = self._card_row(
            [self._row_label("Load address"), self.addr_edit, note, None])
        box.addWidget(self.address_row)

        # BOOT mode: container toggle.
        self.container_switch = Switch()
        self.container_switch.setToolTip(
            "Uses scripts/docker-dev.sh; turn off to use the host's OpenOCD")
        self.container_switch.setChecked(
            DEV_SCRIPT.is_file() and shutil.which("docker") is not None)
        container_note = QLabel("uses scripts/docker-dev.sh")
        container_note.setObjectName("Hint")
        self.container_row = self._card_row(
            [self._row_label("Use stm32-dev container"), container_note, None,
             self.container_switch])
        box.addWidget(self.container_row)

        # Application mode: serial port and baud.
        self.port_combo = QComboBox()
        self.port_combo.setEditable(True)
        self.port_combo.setMinimumWidth(170)
        self.port_combo.currentTextChanged.connect(self._on_port_changed)
        self.refresh_ports_button = QPushButton("Refresh")
        self.refresh_ports_button.setObjectName("Link")
        self.refresh_ports_button.setCursor(Qt.PointingHandCursor)
        self.refresh_ports_button.clicked.connect(self._populate_ports)
        baud_label = QLabel("Baud")
        baud_label.setObjectName("Hint")
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "921600"])
        self.baud_combo.setFixedWidth(100)
        self.serial_row = self._card_row(
            [self._row_label("Serial port"), self.port_combo,
             self.refresh_ports_button, None, baud_label, self.baud_combo],
            stretch_index=1)
        box.addWidget(self.serial_row)

        # Application mode: target slot.
        self.slot_box = QFrame()
        self.slot_box.setObjectName("SegmentBox")
        slot_layout = QHBoxLayout(self.slot_box)
        slot_layout.setContentsMargins(2, 2, 2, 2)
        slot_layout.setSpacing(2)
        self.slot_group = QButtonGroup(self)
        self.slot_group.setExclusive(True)
        self.slot_a = QPushButton("A")
        self.slot_b = QPushButton("B")
        for index, button in enumerate((self.slot_a, self.slot_b)):
            button.setObjectName("Segment")
            button.setCheckable(True)
            button.setCursor(Qt.PointingHandCursor)
            button.setMinimumWidth(52)
            self.slot_group.addButton(button, index)
            slot_layout.addWidget(button)
        self.slot_a.setChecked(True)
        self.slot_group.idClicked.connect(lambda _i: self._on_slot_changed())
        self.slot_hint = QLabel("")
        self.slot_hint.setObjectName("Hint")
        self.target_row = self._card_row(
            [self._row_label("Target slot"), self.slot_box, self.slot_hint,
             None])
        box.addWidget(self.target_row)

        # Application mode: activate after update.
        self.activate_switch = Switch()
        self.activate_switch.setChecked(True)
        activate_note = QLabel("writes ACTIVATED and resets")
        activate_note.setObjectName("Hint")
        self.activate_row = self._card_row(
            [self._row_label("Activate after update"), activate_note, None,
             self.activate_switch])
        box.addWidget(self.activate_row)

        self.verbose_switch = Switch()
        self.verbose_switch.setChecked(True)
        self.verbose_switch.setToolTip(
            "Log every chunk with offset, flash address, rate and a data "
            "preview")
        verbose_note = QLabel("every chunk, offset, flash address and rate")
        verbose_note.setObjectName("Hint")
        self.verbose_row = self._card_row(
            [self._row_label("Verbose output"), verbose_note, None,
             self.verbose_switch])
        box.addWidget(self.verbose_row)

        add_shadow(card)
        return card

    def _production_card(self) -> QFrame:
        """P1-1 (GUI): ordered production workflow with per-step status."""
        card = QFrame()
        card.setObjectName("Card")
        box = QVBoxLayout(card)
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(0)

        header = QHBoxLayout()
        header.setContentsMargins(16, 12, 16, 4)
        title = QLabel("Production workflow")
        title.setObjectName("SectionTitle")
        self.production_hint = QLabel(
            "Build, provision, flash A+B and verify in one guided run")
        self.production_hint.setObjectName("Hint")
        header.addWidget(title)
        header.addStretch(1)
        header.addWidget(self.production_hint)
        box.addLayout(header)

        self.app_repo_edit = QLineEdit()
        self.app_repo_edit.setPlaceholderText(
            "Application repository (C, Rust or Zephyr)")
        self.app_repo_button = QPushButton("Choose…")
        self.app_repo_button.clicked.connect(self._browse_app_repo)
        box.addWidget(self._card_row(
            [self._row_label("Application repo"), self.app_repo_edit,
             self.app_repo_button], stretch_index=1))

        self.app_build_edit = QLineEdit("make")
        self.app_build_edit.setPlaceholderText(
            "build command, run in the application repository")
        box.addWidget(self._card_row(
            [self._row_label("Application build"), self.app_build_edit],
            stretch_index=1))

        self.app_bin_edit = QLineEdit("build/app.bin")
        self.app_bin_edit.setPlaceholderText(
            "application .bin to wrap with mkimage.py (empty = skip)")
        box.addWidget(self._card_row(
            [self._row_label("Application binary"), self.app_bin_edit],
            stretch_index=1))

        self.app_version_edit = QLineEdit("1.0.0")
        box.addWidget(self._card_row(
            [self._row_label("Application version"), self.app_version_edit],
            stretch_index=1))

        self.app_a_edit = QLineEdit(str(DEFAULT_APP_IMG))
        self.app_a_button = QPushButton("Choose…")
        self.app_a_button.clicked.connect(lambda: self._browse_prod_image("a"))
        box.addWidget(self._card_row(
            [self._row_label("Slot A image"), self.app_a_edit,
             self.app_a_button], stretch_index=1))

        self.app_b_edit = QLineEdit(str(DEFAULT_APP_IMG))
        self.app_b_button = QPushButton("Choose…")
        self.app_b_button.clicked.connect(lambda: self._browse_prod_image("b"))
        box.addWidget(self._card_row(
            [self._row_label("Slot B image"), self.app_b_edit,
             self.app_b_button], stretch_index=1))

        self.boot_build_edit = QLineEdit("make build")
        box.addWidget(self._card_row(
            [self._row_label("BOOT build"), self.boot_build_edit],
            stretch_index=1))

        self.provision_edit = QLineEdit("tools/provision.sh")
        box.addWidget(self._card_row(
            [self._row_label("Provision command"), self.provision_edit],
            stretch_index=1))

        self.prod_port_edit = QLineEdit()
        self.prod_port_edit.setPlaceholderText(
            "ST-LINK VCP for steps 5 and 6 (e.g. /dev/ttyACM0)")
        box.addWidget(self._card_row(
            [self._row_label("Serial port"), self.prod_port_edit],
            stretch_index=1))

        self.factory_switch = Switch()
        self.factory_switch.setToolTip(
            "The selected BOOT file is a mkimage --factory-hex image; "
            "one SWD program already writes BOOT, slots A+B and metadata")
        box.addWidget(self._card_row(
            [self._row_label("Factory HEX"),
             QLabel("single SWD program, skip serial A+B"), None,
             self.factory_switch], stretch_index=1))
        self.factory_note = QLabel(
            "generate with: tools/mkimage.py --factory-hex ...")
        self.factory_note.setObjectName("Hint")
        box.addWidget(self._card_row(
            [self._row_label(""), self.factory_note, None],
            stretch_index=1))

        steps = [
            ("build_boot", "1. Build BOOT"),
            ("build_app", "2. Build application"),
            ("provision", "3. Provision option bytes"),
            ("flash_boot", "4. Flash BOOT over SWD"),
            ("flash_ab", "5. Flash application to A and B"),
            ("verify", "6. Verify both slots"),
        ]
        for key, label_text in steps:
            status = QLabel("pending")
            status.setObjectName("Hint")
            self.production_steps[key] = status
            box.addWidget(self._card_row(
                [self._row_label(label_text), status, None],
                stretch_index=1))

        buttons = QHBoxLayout()
        buttons.setContentsMargins(16, 8, 16, 12)
        buttons.setSpacing(8)
        self.run_all_button = QPushButton("Run all steps")
        self.run_all_button.setObjectName("Primary")
        self.run_all_button.clicked.connect(self._run_all_production)
        buttons.addWidget(self.run_all_button)
        for key, label_text in (("build_boot", "Build BOOT"),
                                ("build_app", "Build app"),
                                ("provision", "Provision"),
                                ("flash_boot", "Flash BOOT"),
                                ("flash_ab", "Flash A+B"),
                                ("verify", "Verify")):
            button = QPushButton(label_text)
            button.clicked.connect(
                lambda _checked=False, step=key:
                self._run_production_step(step))
            buttons.addWidget(button)
        box.addLayout(buttons)

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
        self.flash_button.clicked.connect(self._flash)

        self.erase_button = QPushButton("Erase")
        self.erase_button.setObjectName("Danger")
        self.erase_button.setMinimumHeight(52)
        self.erase_button.setToolTip("Erase the whole flash (SWD only)")
        self.erase_button.clicked.connect(self._erase)

        self.reset_button = QPushButton("Reset")
        self.reset_button.setMinimumHeight(52)
        self.reset_button.setToolTip("Reset the target (Ctrl+R)")
        self.reset_button.clicked.connect(self._reset)

        self.probe_button = QPushButton("Probe")
        self.probe_button.setMinimumHeight(52)
        self.probe_button.setToolTip("Detect the ST-LINK and the target chip")
        self.probe_button.clicked.connect(self._probe)

        self.enter_button = QPushButton("Enter BOOT")
        self.enter_button.setMinimumHeight(52)
        self.enter_button.setToolTip(
            "Ask the running application to reset into the bootloader")
        self.enter_button.clicked.connect(self._enter_boot)

        for button in (self.flash_button, self.erase_button,
                       self.reset_button, self.probe_button,
                       self.enter_button):
            row.addWidget(button, 1)
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
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setObjectName("Link")
        self.cancel_button.setCursor(Qt.PointingHandCursor)
        self.cancel_button.setVisible(False)
        self.cancel_button.setToolTip("Stop the running command (Esc)")
        self.cancel_button.clicked.connect(self._cancel)
        clear = QPushButton("Clear")
        clear.setObjectName("Link")
        clear.setCursor(Qt.PointingHandCursor)
        header.addWidget(label)
        header.addStretch(1)
        header.addWidget(self.cancel_button)
        header.addWidget(clear)
        box.addLayout(header)

        self.log = QPlainTextEdit()
        self.log.setObjectName("Log")
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        self.log.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.log.setFont(mono_font())
        self.log.setPlaceholderText("Command output will appear here.")
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

    # ---- mode -----------------------------------------------------------

    def _set_mode(self, mode: str, initial: bool = False) -> None:
        if not initial:
            if self.mode in self.firmware_text:
                self.firmware_text[self.mode] = self.file_edit.text()
            if mode == self.mode:
                return
        self.mode = mode
        boot = mode == "boot"
        app = mode == "app"
        prod = mode == "prod"

        self.mode_boot.setChecked(boot)
        self.mode_app.setChecked(app)
        self.mode_prod.setChecked(prod)
        self.page_title.setText(
            "Bootloader" if boot else "Application" if app else "Production")
        self.page_subtitle.setText(
            "Program BOOT over SWD with OpenOCD" if boot else
            "Update an application image over HWLINK" if app else
            "Guided build, provision, A+B flash and verification workflow")
        self.erase_button.setVisible(boot)
        self.enter_button.setVisible(app)

        self.firmware_card.setVisible(not prod)
        self.production_card.setVisible(prod)
        self.actions_widget.setVisible(not prod)

        self.address_row.setVisible(boot)
        self.container_row.setVisible(boot)
        self.serial_row.setVisible(app)
        self.target_row.setVisible(app)
        self.activate_row.setVisible(app)
        self.verbose_row.setVisible(app)
        self.addr_edit.setVisible(boot)

        if not prod:
            self._set_firmware_text(self.firmware_text[mode])
        self.file_edit.setPlaceholderText(
            "Drag a .elf, .hex or .bin here, or choose a file" if boot else
            "Drag a .img here, or choose a file (tools/mkimage.py)")
        self.flash_button.setText("Flash" if boot else "Update")
        self.probe_button.setText("Probe" if boot else "Boot info")
        self.probe_button.setToolTip(
            "Detect the ST-LINK and the target chip" if boot else
            "Read the active slot, slot states and flash mode")
        self._refresh_hint()
        self._apply_mode_state()

        if app:
            self._populate_ports()
            self._refresh_boot_info()

    def _busy(self) -> bool:
        return (self.process.state() != QProcess.NotRunning or
                self.runner.busy())

    def _apply_mode_state(self) -> None:
        """Enable/disable widgets for the current mode (not while busy)."""
        busy = self._busy()
        boot = self.mode == "boot"
        app = self.mode == "app"
        prod = self.mode == "prod"
        self.erase_button.setEnabled(boot and not busy)
        for widget in (self.flash_button, self.reset_button,
                       self.probe_button, self.enter_button):
            widget.setEnabled(not busy and not prod)
        self.file_edit.setEnabled(not busy)
        self.choose_button.setEnabled(not busy)
        self.addr_edit.setEnabled(not busy)
        self.container_switch.setEnabled(not busy)
        self.port_combo.setEnabled(not busy)
        self.refresh_ports_button.setEnabled(not busy)
        self.baud_combo.setEnabled(not busy)
        self.activate_switch.setEnabled(not busy)
        self.verbose_switch.setEnabled(not busy)
        for widget in (self.app_repo_edit, self.app_repo_button,
                       self.app_build_edit, self.app_a_edit,
                       self.app_a_button, self.app_b_edit,
                       self.app_b_button, self.boot_build_edit,
                       self.provision_edit, self.run_all_button):
            widget.setEnabled(not busy)
        if app:
            self.slot_a.setEnabled(False)
            self.slot_b.setEnabled(False)
        else:
            self.slot_a.setEnabled(not busy)
            self.slot_b.setEnabled(not busy)

    # ---- state / settings ----------------------------------------------

    def _load_settings(self, firmware: Path | None) -> None:
        saved_boot = self.settings.value("firmware", "", type=str)
        saved_app = self.settings.value("app_firmware", "", type=str)

        if firmware is not None:
            boot_path = self._display_path(firmware)
        elif saved_boot and self._resolve(saved_boot).is_file():
            boot_path = saved_boot
        elif DEFAULT_ELF.is_file():
            boot_path = self._display_path(DEFAULT_ELF)
        else:
            boot_path = ""

        if saved_app and self._resolve(saved_app).is_file():
            app_path = saved_app
        elif DEFAULT_APP_IMG.is_file():
            app_path = self._display_path(DEFAULT_APP_IMG)
        else:
            app_path = ""

        self.firmware_text = {"boot": boot_path, "app": app_path}
        self.container_switch.setChecked(
            self.settings.value("docker", self.container_switch.isChecked(),
                                type=bool))
        self.addr_edit.setText(
            self.settings.value("address", DEFAULT_ADDR, type=str))
        self.activate_switch.setChecked(
            self.settings.value("activate", True, type=bool))
        self.verbose_switch.setChecked(
            self.settings.value("verbose", True, type=bool))
        baud = self.settings.value("baud", "115200", type=str)
        self.baud_combo.setCurrentText(baud)
        self.app_repo_edit.setText(
            self.settings.value("app_repo", "", type=str))
        self.app_build_edit.setText(
            self.settings.value("app_build", "make", type=str))
        self.app_bin_edit.setText(
            self.settings.value("app_bin", "build/app.bin", type=str))
        self.app_version_edit.setText(
            self.settings.value("app_version", "1.0.0", type=str))
        self.app_a_edit.setText(
            self.settings.value("prod_a", str(DEFAULT_APP_IMG), type=str))
        self.app_b_edit.setText(
            self.settings.value("prod_b", str(DEFAULT_APP_IMG), type=str))
        self.boot_build_edit.setText(
            self.settings.value("boot_build", "make build", type=str))
        self.provision_edit.setText(
            self.settings.value("provision", "tools/provision.sh", type=str))
        self.prod_port_edit.setText(
            self.settings.value("port", "", type=str))
        self.factory_switch.setChecked(
            self.settings.value("factory_hex", False, type=bool))

    def _save_settings(self) -> None:
        if self.mode in self.firmware_text:
            self.firmware_text[self.mode] = self.file_edit.text().strip()
        self.settings.setValue("firmware", self.firmware_text["boot"])
        self.settings.setValue("app_firmware", self.firmware_text["app"])
        self.settings.setValue("docker", self.container_switch.isChecked())
        self.settings.setValue("address", self.addr_edit.text().strip())
        self.settings.setValue("activate", self.activate_switch.isChecked())
        self.settings.setValue("verbose", self.verbose_switch.isChecked())
        self.settings.setValue("baud", self.baud_combo.currentText())
        self.settings.setValue("port", self.port_combo.currentText())
        self.settings.setValue("app_repo", self.app_repo_edit.text().strip())
        self.settings.setValue("app_build", self.app_build_edit.text())
        self.settings.setValue("app_bin", self.app_bin_edit.text())
        self.settings.setValue("app_version", self.app_version_edit.text())
        self.settings.setValue("prod_a", self.app_a_edit.text())
        self.settings.setValue("prod_b", self.app_b_edit.text())
        self.settings.setValue("boot_build", self.boot_build_edit.text())
        self.settings.setValue("provision", self.provision_edit.text())
        self.settings.setValue("port", self.prod_port_edit.text().strip() or
                               self.port_combo.currentText())
        self.settings.setValue("factory_hex", self.factory_switch.isChecked())

    def closeEvent(self, event) -> None:  # noqa: N802 (Qt API)
        self._save_settings()
        for process in (self.process, self.info_process):
            if process.state() != QProcess.NotRunning:
                process.kill()
                process.waitForFinished(1000)
        super().closeEvent(event)

    def _set_state(self, state: str, text: str) -> None:
        self.status_chip.setProperty("state", state)
        self.status_chip.setText(text)
        self.status_chip.style().unpolish(self.status_chip)
        self.status_chip.style().polish(self.status_chip)

    def _set_busy(self, busy: bool, text: str) -> None:
        self.progress.setVisible(busy)
        self.status.setText(text)
        self.cancel_button.setEnabled(busy)
        self.cancel_button.setVisible(busy)
        if busy:
            for widget in (self.flash_button, self.erase_button,
                           self.reset_button, self.probe_button,
                           self.enter_button,
                           self.file_edit, self.choose_button,
                           self.addr_edit, self.container_switch,
                           self.port_combo, self.refresh_ports_button,
                           self.baud_combo, self.slot_a, self.slot_b,
                           self.activate_switch, self.app_repo_edit,
                           self.app_repo_button, self.app_build_edit,
                           self.app_bin_edit, self.app_version_edit,
                           self.app_a_edit, self.app_a_button,
                           self.app_b_edit, self.app_b_button,
                           self.boot_build_edit, self.provision_edit,
                           self.run_all_button, self.prod_port_edit,
                           self.factory_switch):
                widget.setEnabled(False)
            self._set_state("busy", "Working…")
        else:
            self.progress.setRange(0, 0)
            self._apply_mode_state()

    def _set_firmware_text(self, text: str) -> None:
        self.file_edit.setText(text)
        self.file_edit.setCursorPosition(0)

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
        except OSError:
            self.file_hint.setText(path.name)
            return

        if self.mode == "boot":
            self.file_hint.setText(f"{path.name}  ·  {human_size(stat.st_size)}"
                                   f"  ·  {stamp}")
        else:
            summary = image_summary(path)
            if summary is None:
                self.file_hint.setText(f"{path.name}  ·  not a BOOT image")
            else:
                self.file_hint.setText(f"{path.name}  ·  {summary}")

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

    # ---- serial / boot info --------------------------------------------

    def _populate_ports(self) -> None:
        current = self.port_combo.currentText().strip()
        ports = stlink_ports()
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        saved = self.settings.value("port", "", type=str)
        if current:
            self.port_combo.setCurrentText(current)
        elif saved:
            self.port_combo.setCurrentText(saved)
        elif ports:
            self.port_combo.setCurrentIndex(0)
        self.port_combo.blockSignals(False)

    def _on_port_changed(self, _text: str) -> None:
        self.active_slot = None
        self.flash_mode = None
        self._on_slot_changed()
        if self.mode == "app":
            self.port_timer.start()

    def _refresh_boot_info(self, announce: bool = False) -> None:
        if self.mode != "app":
            return
        if self.info_process.state() != QProcess.NotRunning:
            return
        port = self.port_combo.currentText().strip()
        if not port:
            self.slot_hint.setStyleSheet("")
            self.slot_hint.setText("No serial port found")
            return
        if self.process.state() != QProcess.NotRunning:
            return

        args = [str(BOOTCTL), "--port", port, "--timeout", "3",
                "info", "--json"]
        if announce:
            self.log.appendPlainText(
                f"$ bootctl.py --port {port} info --json")
        self.slot_hint.setStyleSheet("")
        self.slot_hint.setText("Reading boot info…")
        self.info_process.start(sys.executable, args)

    def _on_info_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        stdout = bytes(self.info_process.readAllStandardOutput()).decode(
            "utf-8", "replace")
        stderr = bytes(self.info_process.readAllStandardError()).decode(
            "utf-8", "replace")
        data = None
        if status == QProcess.NormalExit and code == 0:
            try:
                data = json.loads(stdout)
            except json.JSONDecodeError:
                data = None
        self._apply_boot_info(data, stderr.strip())

    def _on_info_error(self, error: QProcess.ProcessError) -> None:
        if error == QProcess.FailedToStart:
            self._apply_boot_info(None, "could not start bootctl.py")

    def _apply_boot_info(self, data: dict | None,
                        error: str = "") -> None:
        if self.mode != "app":
            return
        if data is None:
            self.active_slot = None
            self.target_slot = None
            self.flash_mode = None
            self.boot_version = None
            self.slot_a.setEnabled(False)
            self.slot_b.setEnabled(False)
            self.slot_a.setChecked(True)
            self.slot_hint.setStyleSheet("")
            self.slot_hint.setText(
                "Boot info unavailable · BOOT chooses the target slot")
            if error:
                self.status.setText("Boot info unavailable")
                self._append(error if error.startswith("bootctl:")
                             else f"bootctl: {error}")
            return

        view = parse_boot_info(data)
        self.active_slot = view.active_slot if view else None
        self.target_slot = view.target_slot if view else None
        self.flash_mode = view.flash_mode if view else ""
        self.boot_version = view.boot_version if view else ""

        # P1-3: the target is read-only and comes from BOOT, not from a
        # guess based on the active slot.
        self.slot_a.setEnabled(False)
        self.slot_b.setEnabled(False)
        (self.slot_b if self.target_slot == "B" else
         self.slot_a).setChecked(True)
        self._on_slot_changed()

        if self.flash_mode and self.flash_mode != "DUAL_BANK":
            self.slot_hint.setStyleSheet(f"color: {WARN_COLOR};")
            self.slot_hint.setText(
                "Single-bank flash · run tools/provision.sh for A/B")
        self.status.setText("Ready")
        self._append(
            f"boot info: active {self.active_slot or 'none'}, "
            f"target {self.target_slot or '?'}, "
            f"flash mode {self.flash_mode or 'unknown'}"
            + (f", BOOT {self.boot_version}" if self.boot_version else ""))

    def _on_slot_changed(self) -> None:
        if self.mode != "app":
            return
        target = self.target_slot or "?"
        if self.flash_mode and self.flash_mode != "DUAL_BANK":
            self.slot_hint.setStyleSheet(f"color: {WARN_COLOR};")
            self.slot_hint.setText(
                "Single-bank flash · run tools/provision.sh for A/B")
        elif self.target_slot is None:
            self.slot_hint.setStyleSheet("")
            self.slot_hint.setText(
                "Boot info unavailable · BOOT chooses the target slot")
        else:
            self.slot_hint.setStyleSheet("")
            self.slot_hint.setText(
                f"Active: {self.active_slot or 'none'} · BOOT targets "
                f"{target} (read-only)")

    # ---- process helpers ------------------------------------------------

    def _append(self, text: str) -> None:
        text = ANSI_RE.sub("", text).rstrip("\r\n")
        if not text:
            return
        if text.startswith("{"):
            try:
                event = json.loads(text)
            except json.JSONDecodeError:
                event = None
            if isinstance(event, dict):
                if event.get("event") == "progress":
                    self.progress.setRange(0, 100)
                    percent = event.get("percent")
                    if isinstance(percent, (int, float)):
                        self.progress.setValue(max(0, min(100,
                                                         int(percent))))
                    operation = event.get("operation", "update")
                    self.status.setText(f"{operation} "
                                        f"{event.get('done', 0)}/"
                                        f"{event.get('total', 0)}")
                elif event.get("event") == "error":
                    self._set_state("error", "Failed")
                    self.status.setText(str(event.get("error", "error")))
                elif event.get("event") in ("stage", "result", "confirm"):
                    self.status.setText(str(event.get("status",
                                                     event.get("state",
                                                             "working"))))
                self.log.appendPlainText(text)
                return
        if self.progress.maximum() == 100:
            match = PERCENT_RE.search(text)
            if match:
                self.progress.setValue(
                    min(100, int(float(match.group(1)))))
        self.log.appendPlainText(text)

    def _drain(self) -> None:
        # MergedChannels forwards stdout and stderr to the stdout channel.
        buf = self.process.readAllStandardOutput()
        for line in bytes(buf).decode("utf-8", "replace").splitlines():
            self._append(line)

    def _start(self, program: str, args: list[str], text: str,
               display: str | None = None, step: str | None = None,
               cwd: Path | None = None) -> None:
        if self._busy():
            return
        self.current_step = step
        if cwd is not None:
            self.process.setWorkingDirectory(str(cwd))
        else:
            self.process.setWorkingDirectory(str(ROOT))
        self.log.appendPlainText(
            "$ " + (display or " ".join([program, *args])))
        self.progress.setRange(0, 0)
        self._set_busy(True, text)
        self.process.start(program, args)

    def _project_path(self, path: Path) -> str:
        """Return the host path; docker-dev.sh also mounts the workspace."""
        path = path.resolve()
        workspace = ROOT.parent
        try:
            path.relative_to(workspace)
        except ValueError as exc:
            raise RuntimeError(
                f"{path} is outside the workspace ({workspace}); the "
                "container cannot see it") from exc
        return str(path)

    def _openocd_cfg(self) -> str:
        return os.environ.get("OPENOCD_CFG", "board/st_nucleo_f7.cfg")

    def _dev(self, *args: str) -> None:
        self._start("bash", [str(DEV_SCRIPT), *args], "Running…")

    def _bootctl(self, *args: str, text: str, verbose: bool = False,
                 step: str | None = None) -> None:
        port = self.port_combo.currentText().strip()
        baud = self.baud_combo.currentText().strip() or "115200"
        command = ["--port", port, "--baud", baud]
        if verbose:
            command.append("--verbose")
        command.extend(args)
        self._start(sys.executable, [str(BOOTCTL), *command], text,
                    display="bootctl.py " + " ".join(command), step=step)

    # ---- production workflow -------------------------------------------

    def _browse_app_repo(self) -> None:
        name = QFileDialog.getExistingDirectory(
            self, "Choose the application repository", str(ROOT.parent))
        if name:
            self.app_repo_edit.setText(name)

    def _browse_prod_image(self, slot: str) -> None:
        name, _ = QFileDialog.getOpenFileName(
            self, f"Choose the slot {slot.upper()} image",
            str(DEFAULT_APP_IMG.parent),
            "Application image (*.img);;All files (*)")
        if name:
            (self.app_a_edit if slot == "a" else self.app_b_edit).setText(name)

    def _set_step(self, step: str, state: str, text: str) -> None:
        label = self.production_steps.get(step)
        if label is None:
            return
        colors = {"pending": "#86868b", "running": "#0071e3",
                  "ok": "#248a3d", "error": "#d70015"}
        label.setStyleSheet(f"color: {colors.get(state, '#86868b')};")
        label.setText(text)

    def _production_job(self, step: str) -> dict | None:
        app_repo = Path(self.app_repo_edit.text().strip()).expanduser()
        if self.app_repo_edit.text().strip() and not app_repo.is_dir():
            raise RuntimeError(f"application repository not found: {app_repo}")
        boot_path = None
        if step in ("provision", "flash_boot"):
            boot_path = self._firmware()
            if boot_path is None:
                raise RuntimeError("choose a BOOT firmware file first")
        port = self.prod_port_edit.text().strip() or \
            self.port_combo.currentText().strip()
        baud = self.baud_combo.currentText().strip() or "115200"

        if step == "build_boot":
            command = self.boot_build_edit.text().strip() or "make build"
            if not self.container_switch.isChecked() and \
                    command == "make build":
                command = ("cmake -S . -B build -G Ninja "
                           "-DCMAKE_BUILD_TYPE=Release "
                           "&& cmake --build build")
            return {"program": "bash", "args": ["-lc", command],
                    "cwd": ROOT, "text": "Building BOOT…"}
        def app_path(text: str) -> Path:
            path = Path(text).expanduser()
            if path.is_absolute():
                return path
            base = app_repo if self.app_repo_edit.text().strip() else ROOT
            return base / path

        if step == "build_app":
            command = self.app_build_edit.text().strip()
            if not command or not self.app_repo_edit.text().strip():
                return None
            binary = self.app_bin_edit.text().strip()
            if binary:
                image_a = app_path(self.app_a_edit.text())
                image_b = app_path(self.app_b_edit.text())
                binary_path = app_path(binary)
                version = self.app_version_edit.text().strip() or "1.0.0"
                mkimage = (f"{sys.executable} {ROOT / 'tools' / 'mkimage.py'} "
                           f"--input '{binary_path}' --version '{version}' "
                           f"--timestamp $(date +%s) --name app")
                command = (f"{command} && {mkimage} --output '{image_a}'"
                           f" && {mkimage} --output '{image_b}'")
            return {"program": "bash", "args": ["-lc", command],
                    "cwd": app_repo, "text": "Building application…"}
        if step == "provision":
            command = self.provision_edit.text().strip() or \
                "tools/provision.sh"
            return {"program": "bash",
                    "args": ["-lc", f"{command} '{boot_path}'".strip()],
                    "cwd": ROOT, "text": "Provisioning option bytes…"}
        if step == "flash_boot":
            if self.container_switch.isChecked():
                return {"program": "bash",
                        "args": [str(DEV_SCRIPT), "flash",
                                 str(boot_path)],
                        "cwd": ROOT, "text": "Flashing BOOT…"}
            args = ["-f", self._openocd_cfg(), "-c"]
            target = f'"{boot_path}"'
            if boot_path.suffix.lower() == ".bin":
                target += f" {self.addr_edit.text().strip() or DEFAULT_ADDR}"
            return {"program": "openocd",
                    "args": args + [f"program {target} verify reset exit"],
                    "cwd": ROOT, "text": "Flashing BOOT…"}
        if step == "flash_ab":
            if not port:
                raise RuntimeError("choose the ST-LINK serial port first")
            image_a = app_path(self.app_a_edit.text()) \
                if self.app_a_edit.text().strip() else None
            image_b = app_path(self.app_b_edit.text()) \
                if self.app_b_edit.text().strip() else None
            if image_a is None or not image_a.is_file() or \
                    image_b is None or not image_b.is_file():
                raise RuntimeError("choose the slot A and slot B images first")
            command = [str(BOOTCTL), "--port", port, "--baud", baud,
                       "update", "--both", "--a", str(image_a),
                       "--b", str(image_b), "--json"]
            return {"program": sys.executable, "args": command,
                    "cwd": ROOT, "text": "Updating slots A and B…"}
        if step == "verify":
            if not port:
                raise RuntimeError("choose the ST-LINK serial port first")
            command = [str(BOOTCTL), "--port", port, "--baud", baud,
                       "info", "--json"]
            return {"program": sys.executable, "args": command,
                    "cwd": ROOT, "text": "Verifying both slots…"}
        return None

    def _enqueue_production(self, steps: list[str]) -> None:
        if self._busy():
            return
        jobs: list[Job] = []
        for step in steps:
            try:
                job = self._production_job(step)
            except RuntimeError as error:
                QMessageBox.critical(self, "BOOT Flasher", str(error))
                self._set_step(step, "error", str(error))
                return
            if job is None:
                self._set_step(step, "pending", "skipped")
                continue
            jobs.append(Job(
                step=step,
                program=job["program"],
                args=job["args"],
                cwd=job.get("cwd"),
                text=job["text"],
                display=" ".join([job["program"], *job["args"]])))
        if jobs:
            self.runner.start(jobs)

    def _run_all_production(self) -> None:
        steps = ["build_boot", "build_app", "provision", "flash_boot"]
        if not self.factory_switch.isChecked():
            steps.append("flash_ab")
        steps.append("verify")
        self._enqueue_production(steps)

    def _run_production_step(self, step: str) -> None:
        self._enqueue_production([step])

    def _on_runner_started(self, step: str, text: str) -> None:
        self._set_step(step, "running", "running…")
        self.progress.setRange(0, 0)
        self._set_busy(True, text)
        self._set_state("busy", "Working…")

    def _on_runner_finished(self, step: str, success: bool, code: int) -> None:
        if success:
            self._set_step(step, "ok", "done")
        else:
            self._set_step(step, "error", f"failed ({code})")

    def _on_runner_all_finished(self, failed: bool) -> None:
        if failed:
            self._set_busy(False, "Failed")
            self._set_state("error", "Failed")
        else:
            self._set_busy(False, "Done")
            self._set_state("ok", "Done")

    def _enter_boot(self) -> None:
        port = self.port_combo.currentText().strip()
        if not port:
            QMessageBox.warning(self, "BOOT Flasher",
                                "Choose the ST-LINK serial port first.")
            return
        self._bootctl("enter", text="Entering BOOT…")

    # ---- actions --------------------------------------------------------

    def _browse(self) -> None:
        text = self.file_edit.text().strip()
        start = self._resolve(text) if text else None
        default_dir = (DEFAULT_APP_IMG if self.mode == "app" else DEFAULT_ELF)
        default_dir = default_dir.parent
        start_dir = start.parent if start and start.parent.is_dir() else (
            default_dir if default_dir.is_dir() else ROOT)
        if self.mode == "boot":
            filters = "Firmware (*.elf *.hex *.bin);;All files (*)"
        else:
            filters = "Application image (*.img);;All files (*)"
        name, _ = QFileDialog.getOpenFileName(
            self, "Choose firmware", str(start_dir), filters)
        if name:
            self._set_firmware_text(self._display_path(Path(name)))

    def _firmware(self) -> Path | None:
        if not self.file_edit.text().strip():
            path = None
        else:
            path = self._resolve(self.file_edit.text())
        suffixes = BOOT_SUFFIXES if self.mode == "boot" else APP_SUFFIXES
        wanted = ".elf, .hex or .bin" if self.mode == "boot" else ".img"
        if path is None or not path.is_file():
            QMessageBox.warning(self, "BOOT Flasher",
                                "Choose an existing firmware file first.")
            return None
        if path.suffix.lower() not in suffixes:
            QMessageBox.warning(self, "BOOT Flasher",
                                f"Firmware must be {wanted}.")
            return None
        return path

    def _flash(self) -> None:
        if self.mode == "app":
            self._update_application()
        else:
            self._flash_boot()

    def _flash_boot(self) -> None:
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

    def _update_application(self) -> None:
        firmware = self._firmware()
        if firmware is None:
            return
        port = self.port_combo.currentText().strip()
        if not port:
            QMessageBox.warning(self, "BOOT Flasher",
                                "Choose the ST-LINK serial port first.")
            return
        target = self.target_slot or "?"

        if self.flash_mode and self.flash_mode != "DUAL_BANK":
            answer = QMessageBox.warning(
                self, "Single-bank flash",
                "The board reports SINGLE_BANK flash mode. A/B updates need "
                "dual-bank mode (tools/provision.sh). Continue anyway?",
                QMessageBox.Cancel | QMessageBox.Ok, QMessageBox.Cancel)
            if answer != QMessageBox.Ok:
                return

        args = ["update", str(firmware.resolve()), "--json"]
        if self.activate_switch.isChecked():
            args.extend(["--activate", "--wait"])
        self._bootctl(*args, text=f"Updating slot {target}…")
        if self.process.state() != QProcess.NotRunning:
            self.progress.setRange(0, 100)
            self.progress.setValue(0)

    def _erase(self) -> None:
        if self.mode != "boot":
            return
        if QMessageBox.question(
                self, "Erase flash",
                "Erase the whole flash? BOOT and both application slots "
                "will be lost.") != QMessageBox.Yes:
            return
        if self.container_switch.isChecked():
            self._dev("erase")
        else:
            self._start("openocd", ["-f", self._openocd_cfg(), "-c",
                                    "init; reset halt; stm32f2x mass_erase 0; exit"],
                        "Erasing…")

    def _reset(self) -> None:
        if self.mode == "app":
            if not self.port_combo.currentText().strip():
                QMessageBox.warning(self, "BOOT Flasher",
                                    "Choose the ST-LINK serial port first.")
                return
            self._bootctl("reset", text="Resetting…")
        elif self.container_switch.isChecked():
            self._dev("reset")
        else:
            self._start("openocd", ["-f", self._openocd_cfg(), "-c",
                                    "init; reset run; exit"], "Resetting…")

    def _probe(self) -> None:
        if self.mode == "app":
            self._refresh_boot_info(announce=True)
            return
        if self.container_switch.isChecked():
            self._dev("probe")
        elif shutil.which("st-info") is None:
            QMessageBox.critical(self, "BOOT Flasher",
                                 "st-info not found in PATH "
                                 "(see scripts/install-tools.sh)")
        else:
            self._start("st-info", ["--probe"], "Probing…")

    def _cancel(self) -> None:
        self.runner.cancel()
        if self.process.state() != QProcess.NotRunning:
            self.status.setText("Cancelling…")
            self.process.kill()
        if self.info_process.state() != QProcess.NotRunning:
            self.info_process.kill()

    # ---- drag and drop ---------------------------------------------------

    def dragEnterEvent(self, event) -> None:  # noqa: N802 (Qt API)
        if self._dropped_firmware(event) is not None:
            event.acceptProposedAction()

    def dropEvent(self, event) -> None:  # noqa: N802 (Qt API)
        path = self._dropped_firmware(event)
        if path is not None:
            self._set_firmware_text(self._display_path(path))
            event.acceptProposedAction()

    def _dropped_firmware(self, event) -> Path | None:
        urls = event.mimeData().urls()
        if not urls:
            return None
        path = Path(urls[0].toLocalFile())
        suffixes = BOOT_SUFFIXES if self.mode == "boot" else APP_SUFFIXES
        if path.is_file() and path.suffix.lower() in suffixes:
            return path
        return None

    # ---- process callbacks ----------------------------------------------

    def _on_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        self._drain()
        success = status == QProcess.NormalExit and code == 0
        if success:
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
    parser = argparse.ArgumentParser(
        description="Flash BOOT over SWD or an application image over HWLINK")
    parser.add_argument("-f", "--firmware", type=Path,
                        default=DEFAULT_ELF if DEFAULT_ELF.is_file() else None,
                        help="BOOT firmware to flash (default: build/boot.elf)")
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
