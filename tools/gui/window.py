"""Main window of the flash GUI.

Sidebar: the bootloader and the five firmware projects, then the board
panel (serial port and what BOOT reports).  Content: the selected page above
a shared output console.  One job runner serves every page, because there is
one board.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from dataclasses import dataclass

from PySide6.QtCore import QProcess, QSettings, Qt, QTimer, QUrl
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QDesktopServices,
    QGuiApplication,
    QKeySequence,
    QTextCursor,
)
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSplitter,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from . import icons, projects, theme
from .device import parse_boot_info, stlink_ports
from .pages import BOOTCTL, ROOT, BootloaderPage, FirmwarePage, Page, badge_pixmap
from .runner import Job, Runner
from .widgets import ElidedLabel, NavItem, label, set_tone

APP_NAME = "BOOT Flasher"
VERSION = "1.0"
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
SIGNING_GUIDE = ROOT / "docs" / "IMAGE_SIGNING.md"


@dataclass
class BoardState:
    connected: bool = False
    app_running: bool = False     # the application answered, not BOOT
    error: str = ""
    boot_version: str = ""
    active_slot: str | None = None
    target_slot: str | None = None
    flash_mode: str = ""
    key_id: str = ""
    security_floor: int | None = None


class MainWindow(QMainWindow):
    def __init__(self, boot_firmware: str | None = None) -> None:
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.setMinimumSize(1040, 720)
        self.settings = QSettings("soccentric", "boot-flasher")
        self.board = BoardState()
        self.run_page: Page | None = None
        self._step_started: dict[str, float] = {}
        self._run_failed = False

        self.runner = Runner(self)
        self.runner.output.connect(self._on_output)
        self.runner.started.connect(self._on_step_started)
        self.runner.finished.connect(self._on_step_finished)
        self.runner.allFinished.connect(self._on_run_finished)

        self.info_process = QProcess(self)
        self.info_process.finished.connect(self._on_info_finished)
        self.info_process.errorOccurred.connect(self._on_info_error)
        self.info_timer = QTimer(self)
        self.info_timer.setSingleShot(True)
        self.info_timer.setInterval(400)
        self.info_timer.timeout.connect(self.refresh_board)

        self.pages: list[Page] = [BootloaderPage(self)]
        self.pages += [FirmwarePage(self, project)
                       for project in projects.PROJECTS]

        self._build_ui()
        self._build_menus()
        self._load_settings(boot_firmware)
        self.apply_theme()
        for page in self.pages:
            page.refresh()
        self._populate_ports()
        self.switch_page(self._initial_page)
        hints = QGuiApplication.styleHints()
        hints.colorSchemeChanged.connect(lambda _scheme: self.apply_theme())

    # ---- layout ---------------------------------------------------------

    def _build_ui(self) -> None:
        root = QWidget()
        root.setObjectName("Root")
        row = QHBoxLayout(root)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(0)
        row.addWidget(self._sidebar())

        content = QWidget()
        content.setObjectName("Content")
        column = QVBoxLayout(content)
        column.setContentsMargins(0, 0, 0, 16)
        column.setSpacing(0)

        self.stack = QStackedWidget()
        for page in self.pages:
            scroll = QScrollArea()
            scroll.setObjectName("PageScroll")
            scroll.setWidgetResizable(True)
            scroll.setFrameShape(QFrame.NoFrame)
            scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
            scroll.setWidget(page)
            self.stack.addWidget(scroll)

        self.splitter = QSplitter(Qt.Vertical)
        self.splitter.setChildrenCollapsible(False)
        self.splitter.setHandleWidth(8)
        self.splitter.addWidget(self.stack)
        self.splitter.addWidget(self._console())
        self.splitter.setStretchFactor(0, 3)
        self.splitter.setStretchFactor(1, 2)
        column.addWidget(self.splitter, 1)
        row.addWidget(content, 1)
        self.setCentralWidget(root)

    def _sidebar(self) -> QWidget:
        side = QWidget()
        side.setObjectName("Sidebar")
        side.setFixedWidth(248)
        column = QVBoxLayout(side)
        column.setContentsMargins(12, 16, 12, 12)
        column.setSpacing(2)

        head = QHBoxLayout()
        head.setContentsMargins(8, 0, 8, 0)
        head.setSpacing(12)
        self.app_badge = QLabel()
        self.app_badge.setFixedSize(36, 36)
        titles = QVBoxLayout()
        titles.setSpacing(0)
        titles.addWidget(label(APP_NAME, "AppTitle"))
        titles.addWidget(label("NUCLEO-F767ZI", "AppSubtitle"))
        head.addWidget(self.app_badge)
        head.addLayout(titles, 1)
        column.addLayout(head)
        column.addSpacing(24)

        def section(text: str) -> None:
            heading = label(text, "SidebarSection")
            heading.setContentsMargins(8, 0, 0, 4)
            column.addWidget(heading)

        section("BOOTLOADER")
        for index, page in enumerate(self.pages):
            if index == 1:
                column.addSpacing(16)
                section("APPLICATIONS")
            page.nav = NavItem(page.icon_name, page.title, page.nav_subtitle)
            page.nav.clicked.connect(
                lambda _checked=False, i=index: self.switch_page(i))
            column.addWidget(page.nav)
        column.addStretch(1)
        column.addWidget(self._board_box())
        return side

    def _board_box(self) -> QFrame:
        box = QFrame()
        box.setObjectName("DeviceBox")
        column = QVBoxLayout(box)
        column.setContentsMargins(12, 10, 8, 12)
        column.setSpacing(8)

        head = QHBoxLayout()
        head.setSpacing(4)
        head.addWidget(label("Board", "CardTitle"))
        head.addStretch(1)
        self.board_pill = label("", "Pill")
        head.addWidget(self.board_pill)
        self.board_refresh = QPushButton()
        self.board_refresh.setObjectName("IconButton")
        self.board_refresh.setToolTip("Refresh ports and board info (F5)")
        self.board_refresh.clicked.connect(self.refresh_all_board)
        head.addWidget(self.board_refresh)
        column.addLayout(head)

        self.port_combo = QComboBox()
        self.port_combo.setEditable(True)
        self.port_combo.lineEdit().setPlaceholderText("No ST-LINK found")
        self.port_combo.setToolTip("ST-LINK virtual COM port")
        self.port_combo.currentTextChanged.connect(self._on_port_changed)
        column.addWidget(self.port_combo)
        self.board_line1 = ElidedLabel(name="Hint", mode=Qt.ElideRight)
        self.board_line2 = ElidedLabel(name="Hint", mode=Qt.ElideRight)
        column.addWidget(self.board_line1)
        column.addWidget(self.board_line2)
        return box

    def _console(self) -> QFrame:
        card = QFrame()
        card.setObjectName("Card")
        outer = QVBoxLayout(card)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        head = QHBoxLayout()
        head.setContentsMargins(16, 8, 8, 8)
        head.setSpacing(8)
        self.console_title = label("Output", "CardTitle")
        self.console_status = ElidedLabel(name="Hint", mode=Qt.ElideRight)
        self.progress = QProgressBar()
        self.progress.setObjectName("Progress")
        self.progress.setTextVisible(False)
        self.progress.setFixedWidth(160)
        self.progress.hide()
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setObjectName("Link")
        self.cancel_button.setToolTip("Stop the running command (Esc)")
        self.cancel_button.clicked.connect(self.cancel)
        self.cancel_button.hide()
        self.copy_button = QPushButton()
        self.copy_button.setObjectName("IconButton")
        self.copy_button.setToolTip("Copy the output")
        self.copy_button.clicked.connect(self.copy_output)
        self.clear_button = QPushButton()
        self.clear_button.setObjectName("IconButton")
        self.clear_button.setToolTip("Clear the output (Ctrl+K)")
        self.clear_button.clicked.connect(self.clear_output)
        head.addWidget(self.console_title)
        head.addWidget(self.console_status, 1)
        head.addWidget(self.progress)
        head.addWidget(self.cancel_button)
        head.addWidget(self.copy_button)
        head.addWidget(self.clear_button)
        outer.addLayout(head)

        self.console = QPlainTextEdit()
        self.console.setObjectName("Console")
        self.console.setReadOnly(True)
        self.console.setMaximumBlockCount(20000)
        self.console.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.console.setFont(theme.mono_font())
        self.console.setPlaceholderText(
            "Command output appears here. Choose a page on the left and "
            "press its blue button.")
        self.console.setMinimumHeight(120)
        outer.addWidget(self.console, 1)

        wrapper = QWidget()
        wrapper.setObjectName("Content")
        layout = QVBoxLayout(wrapper)
        layout.setContentsMargins(32, 0, 32, 0)
        layout.addWidget(card)
        return wrapper

    def _build_menus(self) -> None:
        bar = self.menuBar()

        file_menu = bar.addMenu("&File")
        open_action = QAction("Open Bootloader Firmware…", self)
        open_action.setShortcut(QKeySequence.Open)
        open_action.triggered.connect(self._open_boot_firmware)
        file_menu.addAction(open_action)
        file_menu.addSeparator()
        quit_action = QAction("Quit", self)
        quit_action.setShortcut(QKeySequence.Quit)
        quit_action.setMenuRole(QAction.QuitRole)
        quit_action.triggered.connect(self.close)
        file_menu.addAction(quit_action)

        edit_menu = bar.addMenu("&Edit")
        for text, key, method in (("Undo", QKeySequence.Undo, "undo"),
                                  ("Redo", QKeySequence.Redo, "redo"),
                                  (None, None, None),
                                  ("Cut", QKeySequence.Cut, "cut"),
                                  ("Copy", QKeySequence.Copy, "copy"),
                                  ("Paste", QKeySequence.Paste, "paste"),
                                  ("Select All", QKeySequence.SelectAll,
                                   "selectAll")):
            if text is None:
                edit_menu.addSeparator()
                continue
            action = QAction(text, self)
            action.setShortcut(key)
            action.setShortcutContext(Qt.WidgetWithChildrenShortcut)
            action.triggered.connect(
                lambda _checked=False, m=method: self._route_edit(m))
            edit_menu.addAction(action)

        view_menu = bar.addMenu("&View")
        self.page_actions: list[QAction] = []
        group = QActionGroup(self)
        for index, page in enumerate(self.pages):
            action = QAction(page.title, self)
            action.setCheckable(True)
            action.setShortcut(QKeySequence(f"Ctrl+{index + 1}"))
            action.triggered.connect(
                lambda _checked=False, i=index: self.switch_page(i))
            group.addAction(action)
            view_menu.addAction(action)
            self.page_actions.append(action)
            if index == 0:
                view_menu.addSeparator()
        view_menu.addSeparator()
        clear_action = QAction("Clear Output", self)
        clear_action.setShortcut(QKeySequence("Ctrl+K"))
        clear_action.triggered.connect(self.clear_output)
        view_menu.addAction(clear_action)

        run_menu = bar.addMenu("&Run")
        self.run_all_action = QAction("Build and Flash", self)
        self.run_all_action.setShortcut(QKeySequence("Ctrl+Return"))
        self.run_all_action.triggered.connect(
            lambda: self.current_page().run_all())
        build_action = QAction("Build", self)
        build_action.setShortcut(QKeySequence("Ctrl+B"))
        build_action.triggered.connect(
            lambda: self.current_page().run_steps(["build"]))
        flash_action = QAction("Flash Only", self)
        flash_action.setShortcut(QKeySequence("Ctrl+Shift+Return"))
        flash_action.triggered.connect(
            lambda: self.current_page().run_steps(["flash"]))
        self.cancel_action = QAction("Cancel", self)
        self.cancel_action.setShortcut(QKeySequence(Qt.Key_Escape))
        self.cancel_action.triggered.connect(self.cancel)
        self.cancel_action.setEnabled(False)
        for action in (self.run_all_action, build_action, flash_action):
            run_menu.addAction(action)
        run_menu.addSeparator()
        run_menu.addAction(self.cancel_action)
        self.run_actions = [self.run_all_action, build_action, flash_action]

        board_menu = bar.addMenu("&Board")
        refresh_action = QAction("Refresh Board", self)
        refresh_action.setShortcut(QKeySequence.Refresh)
        refresh_action.triggered.connect(self.refresh_all_board)
        board_menu.addAction(refresh_action)
        speed = board_menu.addMenu("Upload Speed")
        self.baud_group = QActionGroup(self)
        for baud in ("115200", "921600"):
            action = QAction(f"{baud} baud", self)
            action.setCheckable(True)
            action.setData(baud)
            self.baud_group.addAction(action)
            speed.addAction(action)
        board_menu.addSeparator()
        enter_action = QAction("Enter Bootloader", self)
        enter_action.triggered.connect(self.enter_boot)
        board_menu.addAction(enter_action)
        reset_action = QAction("Reset Board", self)
        reset_action.setShortcut(QKeySequence("Ctrl+R"))
        reset_action.triggered.connect(
            lambda: self.current_page().reset_board())
        board_menu.addAction(reset_action)
        self.board_actions = [enter_action, reset_action]

        help_menu = bar.addMenu("&Help")
        guide_action = QAction("Image Signing Guide", self)
        guide_action.triggered.connect(
            lambda: QDesktopServices.openUrl(
                QUrl.fromLocalFile(str(SIGNING_GUIDE))))
        help_menu.addAction(guide_action)
        about_action = QAction(f"About {APP_NAME}", self)
        about_action.setMenuRole(QAction.AboutRole)
        about_action.triggered.connect(self._about)
        help_menu.addAction(about_action)

    # ---- theme ----------------------------------------------------------

    def apply_theme(self) -> None:
        t = theme.set_dark(theme.system_is_dark())
        qss = theme.stylesheet(t)
        qss = qss.replace("__CHEVRON_UP__", icons.asset("chevron-up",
                                                        t.text_secondary))
        qss = qss.replace("__CHEVRON__", icons.asset("chevron-down",
                                                     t.text_secondary))
        app = QApplication.instance()
        app.setStyleSheet(qss)
        self.app_badge.setPixmap(badge_pixmap("zap", t.accent, 36))
        self.board_refresh.setIcon(icons.icon("refresh", t.text_secondary))
        self.copy_button.setIcon(icons.icon("copy", t.text_secondary))
        self.clear_button.setIcon(icons.icon("trash", t.text_secondary))
        for page in self.pages:
            page.apply_theme()

    # ---- pages ----------------------------------------------------------

    def current_page(self) -> Page:
        return self.pages[self.stack.currentIndex()]

    def switch_page(self, index: int) -> None:
        """The one place that changes page: nav, menu and title follow."""
        index = max(0, min(index, len(self.pages) - 1))
        self.stack.setCurrentIndex(index)
        page = self.pages[index]
        for i, other in enumerate(self.pages):
            other.nav.setChecked(i == index)
            self.page_actions[i].setChecked(i == index)
        self.setWindowTitle(f"{page.title} — {APP_NAME}")
        self.run_all_action.setText(page.primary_button.text()
                                    .replace("&&", "and"))
        if page.uses_serial and not self.board.connected and \
                not self.board.app_running and \
                self.port() and not self.busy():
            self.info_timer.start()

    def _open_boot_firmware(self) -> None:
        self.switch_page(0)
        self.pages[0].browse()

    # ---- runner ---------------------------------------------------------

    def busy(self) -> bool:
        return self.runner.busy() or self.run_page is not None

    def run(self, page: Page, jobs: list[Job], steps: list[str]) -> None:
        if self.busy() or not jobs:
            return
        if self.info_process.state() != QProcess.NotRunning:
            self.info_process.kill()
            self.info_process.waitForFinished(500)
        self.run_page = page
        self._run_failed = False
        self._step_started.clear()
        for step in steps:
            page.stepper.set_state(step, "pending")
        self.console.appendPlainText("")
        self._set_busy(True)
        self.runner.start(jobs)

    def _set_busy(self, busy: bool) -> None:
        for page in self.pages:
            page.set_busy(busy)
        for action in (*self.run_actions, *self.board_actions):
            action.setEnabled(not busy)
        self.port_combo.setEnabled(not busy)
        self.board_refresh.setEnabled(not busy)
        self.cancel_action.setEnabled(busy)
        self.cancel_button.setVisible(busy)
        if busy:
            self.progress.setRange(0, 0)
            self.progress.show()
        else:
            self.progress.hide()

    def _on_step_started(self, step: str, text: str) -> None:
        self._step_started.setdefault(step, time.monotonic())
        if self.run_page is not None:
            self.run_page.stepper.set_state(step, "running")
        self.progress.setRange(0, 0)
        self.console_status.setText(text)
        set_tone(self.console_status, "")

    def _on_step_finished(self, step: str, success: bool, code: int) -> None:
        page = self.run_page
        elapsed = time.monotonic() - self._step_started.get(step,
                                                             time.monotonic())
        note = f"{elapsed:.0f} s" if elapsed >= 1 else ""
        if page is not None:
            page.stepper.set_state(step, "done" if success else "failed",
                                   note if success else f"exit {code}")

    def _on_run_finished(self, failed: bool) -> None:
        page = self.run_page
        self.run_page = None
        self._set_busy(False)
        if failed:
            self.console_status.setText("Failed · see the output above")
            set_tone(self.console_status, "danger")
        else:
            self.console_status.setText("Done")
            set_tone(self.console_status, "success")
        if page is not None:
            page.run_finished(failed)
            if page.uses_serial or failed is False:
                self.info_timer.start()

    def _on_output(self, line: str) -> None:
        text = ANSI_RE.sub("", line).rstrip("\r\n")
        if not text:
            return
        if text.startswith("{"):
            try:
                event = json.loads(text)
            except json.JSONDecodeError:
                event = None
            if isinstance(event, dict):
                self._on_event(event)
                if event.get("event") == "progress":
                    return       # the progress bar shows it
        self._append(text)

    def _on_event(self, event: dict) -> None:
        kind = event.get("event")
        if kind == "progress":
            percent = event.get("percent")
            if isinstance(percent, (int, float)):
                self.progress.setRange(0, 100)
                self.progress.setValue(max(0, min(100, int(percent))))
                if self.run_page is not None:
                    self.run_page.stepper.set_note(
                        self.runner.current.step if self.runner.current
                        else "flash", f"{int(percent)}%")
            operation = event.get("operation", "update")
            self.console_status.setText(
                f"{operation} {event.get('done', 0)} / {event.get('total', 0)}")
        elif kind == "error":
            self.console_status.setText(str(event.get("error", "error")))
            set_tone(self.console_status, "danger")
        elif kind in ("stage", "result", "confirm"):
            self.console_status.setText(str(event.get("status",
                                                      event.get("state", ""))))

    def _append(self, text: str) -> None:
        bar = self.console.verticalScrollBar()
        at_end = bar.value() >= bar.maximum() - 4
        self.console.appendPlainText(text)
        if at_end:
            self.console.moveCursor(QTextCursor.End)
            bar.setValue(bar.maximum())

    def console_line(self, text: str) -> None:
        self._append(text)

    def cancel(self) -> None:
        if not self.busy():
            return
        self.console_status.setText("Cancelling…")
        self.runner.cancel()

    def clear_output(self) -> None:
        self.console.clear()
        if not self.busy():
            self.console_status.setText("")

    def copy_output(self) -> None:
        QGuiApplication.clipboard().setText(self.console.toPlainText())
        self.console_status.setText("Output copied")
        set_tone(self.console_status, "")

    # ---- board ----------------------------------------------------------

    def port(self) -> str:
        return self.port_combo.currentText().strip()

    def baud(self) -> str:
        action = self.baud_group.checkedAction()
        return action.data() if action else "115200"

    def openocd_cfg(self) -> str:
        return os.environ.get("OPENOCD_CFG", "board/st_nucleo_f7.cfg")

    def _populate_ports(self) -> None:
        current = self.port() or self.settings.value("port", "", type=str)
        ports = stlink_ports()
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if current and (current in ports or not ports):
            self.port_combo.setCurrentText(current)
        elif ports:
            self.port_combo.setCurrentIndex(0)
        else:
            self.port_combo.setCurrentText("")
        self.port_combo.blockSignals(False)
        self._on_port_changed(self.port())

    def refresh_all_board(self) -> None:
        if self.busy():
            return
        self._populate_ports()

    def _on_port_changed(self, _text: str) -> None:
        self.board = BoardState()
        self._show_board()
        if self.port():
            self.info_timer.start()

    def refresh_board(self) -> None:
        port = self.port()
        if not port or self.busy() or \
                self.info_process.state() != QProcess.NotRunning:
            return
        self.board_line1.setText("Reading board…")
        set_tone(self.board_line1, "")
        self.board_line2.setText(port)
        # --no-enter: reading the panel must never stop a running
        # application.  Flashing switches to BOOT on its own.
        self.info_process.start(sys.executable, [
            str(BOOTCTL), "--port", port, "--timeout", "2", "--no-enter",
            "info", "--json"])

    def _on_info_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        out = bytes(self.info_process.readAllStandardOutput()).decode(
            "utf-8", "replace")
        err = bytes(self.info_process.readAllStandardError()).decode(
            "utf-8", "replace").strip()
        ok = status == QProcess.NormalExit and code == 0
        if ok and self.absorb_board_json(out):
            return
        message = (err.splitlines()[-1] if err else
                   "no answer").removeprefix("bootctl: ")
        if "running the application" in message:
            # Keep what BOOT told us last; the slots are unchanged.
            self.board.app_running = True
            self.board.connected = False
            self.board.error = ""
        else:
            self.board = BoardState(error=message)
        self._show_board()

    def absorb_board_json(self, text: str) -> bool:
        """Take `bootctl info --json` output; False if it is not that."""
        start, end = text.find("{"), text.rfind("}")
        try:
            data = json.loads(text[start:end + 1]) if start >= 0 else None
        except json.JSONDecodeError:
            data = None
        view = parse_boot_info(data) if isinstance(data, dict) else None
        if view is None:
            return False
        self.board = BoardState(
            connected=True, boot_version=view.boot_version,
            active_slot=view.active_slot, target_slot=view.target_slot,
            flash_mode=view.flash_mode, key_id=view.key_id,
            security_floor=view.security_floor)
        self._show_board()
        return True

    def _on_info_error(self, error: QProcess.ProcessError) -> None:
        if error == QProcess.FailedToStart:
            self.board = BoardState(error="could not start bootctl.py")
            self._show_board()

    def _show_board(self) -> None:
        board = self.board
        if not self.port():
            self.board_pill.setText("No port")
            set_tone(self.board_pill, "")
            self.board_line1.setText("Connect the ST-LINK USB port")
            self.board_line2.setText("")
        elif board.connected:
            single = board.flash_mode and board.flash_mode != "DUAL_BANK"
            self.board_pill.setText("Single bank" if single else "Connected")
            set_tone(self.board_pill, "warning" if single else "success")
            parts = [f"BOOT {board.boot_version}" if board.boot_version
                     else "BOOT"]
            if board.active_slot:
                parts.append(f"slot {board.active_slot} active")
            if board.target_slot:
                parts.append(f"next {board.target_slot}")
            self.board_line1.setText(" · ".join(parts))
            set_tone(self.board_line1, "")
            signing = []
            if board.key_id:
                signing.append(f"key {board.key_id}")
            if board.security_floor is not None:
                signing.append(f"floor {board.security_floor}")
            self.board_line2.setText(" · ".join(signing) or
                                     "unsigned images accepted")
        elif board.app_running:
            self.board_pill.setText("App running")
            set_tone(self.board_pill, "accent")
            known = [f"BOOT {board.boot_version}"] if board.boot_version else []
            if board.active_slot:
                known.append(f"slot {board.active_slot}")
            self.board_line1.setText("Application running" +
                                     (f" · {' · '.join(known)}" if known
                                      else ""))
            set_tone(self.board_line1, "")
            if board.key_id or board.security_floor is not None:
                signing = []
                if board.key_id:
                    signing.append(f"key {board.key_id}")
                if board.security_floor is not None:
                    signing.append(f"floor {board.security_floor}")
                self.board_line2.setText(" · ".join(signing))
            else:
                self.board_line2.setText("Flashing reads BOOT's key and floor")
        elif board.error:
            self.board_pill.setText("No answer")
            set_tone(self.board_pill, "warning")
            self.board_line1.setText(board.error)
            set_tone(self.board_line1, "warning")
            self.board_line2.setText("Is BOOT flashed? Try Refresh.")
        else:
            self.board_pill.setText("")
            self.board_line1.setText("")
            self.board_line2.setText("")
        self.board_pill.setVisible(bool(self.board_pill.text()))
        for page in self.pages:
            page.board_info_changed()

    def serial_command(self, command: str, text: str) -> None:
        port = self.port()
        if not port:
            self.error("Board", "Choose the board's serial port in the "
                       "sidebar first.")
            return
        args = [str(BOOTCTL), "--port", port, "--baud", self.baud(), command]
        self.run(self.current_page(),
                 [Job(command, sys.executable, args, text, ROOT,
                      "bootctl.py " + " ".join(args[1:]))], [])

    def enter_boot(self) -> None:
        self.serial_command("enter", "Switching the board to BOOT…")

    # ---- dialogs --------------------------------------------------------

    def error(self, title: str, text: str) -> None:
        box = QMessageBox(QMessageBox.Warning, title, text, QMessageBox.Ok, self)
        box.exec()

    def confirm(self, title: str, text: str, ok_text: str,
                destructive: bool = False) -> bool:
        box = QMessageBox(QMessageBox.Warning if destructive
                          else QMessageBox.Question, title, text,
                          QMessageBox.NoButton, self)
        ok = box.addButton(ok_text, QMessageBox.AcceptRole)
        cancel = box.addButton("Cancel", QMessageBox.RejectRole)
        if destructive:
            ok.setObjectName("Danger")
        box.setDefaultButton(cancel)
        box.exec()
        return box.clickedButton() is ok

    def _about(self) -> None:
        QMessageBox.about(
            self, f"About {APP_NAME}",
            f"<b>{APP_NAME} {VERSION}</b><br>Flash the A/B bootloader and the "
            "firmware projects of the soccentric-stm32 workspace onto a "
            "NUCLEO-F767ZI.<br><br>Applications are signed with ECDSA P-256 "
            "before upload; BOOT only starts signed images.")

    @staticmethod
    def _route_edit(method: str) -> None:
        widget = QApplication.focusWidget()
        target = getattr(widget, method, None)
        if callable(target):
            target()

    # ---- settings -------------------------------------------------------

    def _load_settings(self, boot_firmware: str | None) -> None:
        geometry = self.settings.value("window/geometry")
        if geometry is not None:
            self.restoreGeometry(geometry)
        else:
            self.resize(1200, 880)
        state = self.settings.value("window/splitter")
        if state is not None:
            self.splitter.restoreState(state)
        else:
            self.splitter.setSizes([560, 240])
        for page in self.pages:
            page.load(self.settings)
        if boot_firmware:
            self.pages[0].firmware_edit.setText(boot_firmware)
        baud = self.settings.value("baud", "115200", type=str)
        for action in self.baud_group.actions():
            action.setChecked(action.data() == baud)
        if self.baud_group.checkedAction() is None:
            self.baud_group.actions()[0].setChecked(True)
        saved = self.settings.value("page", "boot", type=str)
        self._initial_page = next(
            (i for i, page in enumerate(self.pages) if page.key == saved), 0)
        if boot_firmware:
            self._initial_page = 0

    def _save_settings(self) -> None:
        self.settings.setValue("window/geometry", self.saveGeometry())
        self.settings.setValue("window/splitter", self.splitter.saveState())
        self.settings.setValue("page", self.current_page().key)
        self.settings.setValue("port", self.port())
        self.settings.setValue("baud", self.baud())
        for page in self.pages:
            page.save(self.settings)

    def closeEvent(self, event) -> None:
        if self.busy() and not self.confirm(
                "Stop the running command?",
                "A build or flash is still running. Quitting stops it; an "
                "interrupted flash leaves the target slot unusable until "
                "it is flashed again.", "Quit", destructive=True):
            event.ignore()
            return
        self.runner.cancel()
        if self.info_process.state() != QProcess.NotRunning:
            self.info_process.kill()
            self.info_process.waitForFinished(1000)
        self._save_settings()
        super().closeEvent(event)
