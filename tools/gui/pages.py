"""Pages of the flash GUI: the bootloader and one page per firmware project.

A page owns its form, its step tracker and the jobs it can queue.  The main
window owns the job runner, the board connection and the output console;
pages reach them through `self.host`.
"""

from __future__ import annotations

import shutil
import sys
from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QRectF, QSettings, Qt, QUrl
from PySide6.QtGui import QColor, QDesktopServices, QPainter, QPixmap
from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from . import icons, projects, theme
from .image import human_size, image_summary
from .runner import Job
from .widgets import Card, ElidedLabel, StepTracker, Switch, field, label, set_tone

ROOT = projects.ROOT
DEV_SCRIPT = ROOT / "scripts" / "docker-dev.sh"
BOOTCTL = ROOT / "tools" / "bootctl.py"
PROVISION = ROOT / "tools" / "provision.sh"
BOOT_ADDRESS = "0x08000000"
BOOT_SUFFIXES = (".elf", ".hex", ".bin")
GIT_ENV = {"GIT_TERMINAL_PROMPT": "0",
           "GIT_SSH_COMMAND": "ssh -o BatchMode=yes"}


def display_path(path: Path, base: Path = ROOT) -> str:
    try:
        return path.resolve().relative_to(base.resolve()).as_posix()
    except (OSError, ValueError):
        return str(path)


def resolve(text: str, base: Path) -> Path:
    path = Path(text.strip()).expanduser()
    return path if path.is_absolute() else base / path


def stamp(path: Path) -> str:
    try:
        when = datetime.fromtimestamp(path.stat().st_mtime)  # noqa: DTZ006
    except OSError:
        return ""
    if when.date() == datetime.now().date():  # noqa: DTZ005
        return f"built {when:%H:%M}"
    return f"built {when:%d %b %H:%M}"


class Page(QWidget):
    """Shared layout: header, cards, then the pipeline card."""

    key = ""
    title = ""
    subtitle = ""
    icon_name = "app"
    nav_subtitle = ""
    uses_serial = False
    STEPS: tuple[tuple[str, str], ...] = ()

    def __init__(self, host) -> None:
        super().__init__()
        self.setObjectName("Page")
        self.host = host
        self.nav = None
        self._column = QVBoxLayout(self)
        self._column.setContentsMargins(32, 24, 32, 24)
        self._column.setSpacing(16)
        self._column.addLayout(self._header())
        self.stepper = StepTracker(self.STEPS)
        self.action_buttons: list[QPushButton] = []
        self.inputs: list[QWidget] = []

    # ---- layout ---------------------------------------------------------

    def _header(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(16)
        self.badge = QLabel()
        self.badge.setFixedSize(48, 48)
        texts = QVBoxLayout()
        texts.setSpacing(2)
        texts.addWidget(label(self.title, "PageTitle"))
        texts.addWidget(label(self.subtitle, "PageSubtitle"))
        self.state_pill = label("", "Pill")
        row.addWidget(self.badge)
        row.addLayout(texts, 1)
        row.addWidget(self.state_pill, 0, Qt.AlignVCenter)
        return row

    def _pipeline_card(self, primary: str, secondary: list[tuple[str, str]],
                       menu: QMenu) -> Card:
        card = Card()
        card.add_widget(self._padded(self.stepper, 12, 16))

        actions = QHBoxLayout()
        actions.setSpacing(8)
        self.primary_button = QPushButton(primary)
        self.primary_button.setObjectName("Primary")
        self.primary_button.clicked.connect(self.run_all)
        actions.addWidget(self.primary_button)
        self.action_buttons.append(self.primary_button)
        for text, step in secondary:
            button = QPushButton(text)
            button.setObjectName("Secondary")
            button.clicked.connect(
                lambda _checked=False, s=step: self.run_steps([s]))
            actions.addWidget(button)
            self.action_buttons.append(button)
        actions.addStretch(1)
        self.more_button = QPushButton("More")
        self.more_button.setObjectName("Secondary")
        self.more_button.setMenu(menu)
        self.more_button.setToolTip("More board and project actions")
        actions.addWidget(self.more_button)
        self.action_buttons.append(self.more_button)
        holder = QWidget()
        holder.setLayout(actions)
        actions.setContentsMargins(16, 12, 16, 16)
        card.add_widget(holder)
        return card

    @staticmethod
    def _padded(widget: QWidget, vertical: int, horizontal: int) -> QWidget:
        holder = QWidget()
        layout = QVBoxLayout(holder)
        layout.setContentsMargins(horizontal, vertical, horizontal, vertical)
        layout.addWidget(widget)
        return holder

    def add_card(self, card: Card) -> None:
        self._column.addWidget(card)

    def add_pipeline(self, card: Card) -> None:
        """The pipeline sits right under the header, always in view."""
        self._column.insertWidget(1, card)

    def finish_layout(self) -> None:
        self._column.addStretch(1)

    def apply_theme(self) -> None:
        t = theme.current()
        self.badge.setPixmap(badge_pixmap(self.icon_name, t.accent, 48))
        for button, glyph in getattr(self, "_icon_buttons", []):
            button.setIcon(icons.icon(glyph, t.on_accent if
                                      button.objectName() == "Primary"
                                      else t.text, t.text_tertiary))
        self.stepper.update()
        if self.nav is not None:
            self.nav.update()

    def _set_icon(self, button: QPushButton, glyph: str) -> None:
        if not hasattr(self, "_icon_buttons"):
            self._icon_buttons = []
        self._icon_buttons.append((button, glyph))

    # ---- behaviour ------------------------------------------------------

    def set_busy(self, busy: bool) -> None:
        for widget in (*self.action_buttons, *self.inputs):
            widget.setEnabled(not busy)

    def set_pill(self, text: str, tone: str) -> None:
        self.state_pill.setText(text)
        self.state_pill.setVisible(bool(text))
        set_tone(self.state_pill, tone)

    def run_all(self) -> None:
        self.run_steps([key for key, _ in self.STEPS])

    def run_steps(self, steps: list[str]) -> None:
        if self.host.busy():
            return
        try:
            jobs, steps = self.jobs(steps)
        except RuntimeError as error:
            self.host.error(self.title, str(error))
            return
        if jobs:
            self.host.run(self, jobs, steps)

    def jobs(self, steps: list[str]) -> tuple[list[Job], list[str]]:
        raise NotImplementedError

    def refresh(self) -> None:
        """Re-read files and repository state."""

    def board_info_changed(self) -> None:
        """BOOT's key id, floor or target slot changed."""

    def run_finished(self, failed: bool) -> None:
        self.refresh()

    def load(self, settings: QSettings) -> None:
        pass

    def save(self, settings: QSettings) -> None:
        pass


def badge_pixmap(glyph: str, accent: str, size: int) -> QPixmap:
    """Rounded accent tile with a white glyph, like a macOS app icon."""
    ratio = 2.0
    pix = QPixmap(int(size * ratio), int(size * ratio))
    pix.setDevicePixelRatio(ratio)
    pix.fill(Qt.transparent)
    painter = QPainter(pix)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setPen(Qt.NoPen)
    painter.setBrush(QColor(accent))
    painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.24, size * 0.24)
    inset = size * 0.25
    icons.paint(painter, glyph, QRectF(inset, inset, size - 2 * inset,
                                       size - 2 * inset), "#ffffff", 1.8)
    painter.end()
    return pix


# ---------------------------------------------------------------------------


class BootloaderPage(Page):
    key = "boot"
    title = "Bootloader"
    subtitle = "A/B bootloader for the NUCLEO-F767ZI, flashed over SWD."
    icon_name = "chip"
    nav_subtitle = "bootloader"
    STEPS = (("build", "Build"), ("flash", "Flash over SWD"))

    def __init__(self, host) -> None:
        super().__init__(host)
        self.default_firmware = ROOT / "build" / "boot.elf"

        source = Card("Source")
        self.repo_text = ElidedLabel(name="Mono")
        source.add_row("Repository", self.repo_text, stretch=0)

        self.build_edit = QLineEdit("make build")
        self.build_edit.setPlaceholderText("run in the bootloader repository")
        source.add_row("Build command", self.build_edit, stretch=0)

        self.firmware_edit = QLineEdit()
        self.firmware_edit.setPlaceholderText("build/boot.elf (.elf, .hex or .bin)")
        self.firmware_edit.textChanged.connect(self.refresh_firmware)
        self.firmware_hint = ElidedLabel()
        self.choose_button = QPushButton("Choose…")
        self.choose_button.clicked.connect(self.browse)
        source.add_row("Firmware", field(self.firmware_edit, self.firmware_hint),
                       self.choose_button, stretch=0, align_top=True)

        self.container_switch = Switch()
        self.container_switch.setChecked(
            DEV_SCRIPT.is_file() and shutil.which("docker") is not None)
        self.container_switch.toggled.connect(self.refresh)
        self.container_hint = label("", "Hint")
        source.add_row("Build in Docker",
                       self.container_hint, None, self.container_switch,
                       stretch=-1)
        self.add_card(source)

        menu = QMenu(self)
        menu.addAction("Probe ST-LINK", self.probe)
        menu.addAction("Reset board", self.reset_board)
        menu.addSeparator()
        menu.addAction("Provision board…", self.provision)
        menu.addAction("Erase entire flash…", self.erase)
        self.add_pipeline(self._pipeline_card(
            "Build && Flash", [("Build", "build"), ("Flash", "flash")], menu))
        self._set_icon(self.primary_button, "zap")
        self.finish_layout()

        self.inputs = [self.build_edit, self.firmware_edit, self.choose_button,
                       self.container_switch]

    # ---- state ----------------------------------------------------------

    def firmware(self) -> Path | None:
        text = self.firmware_edit.text().strip()
        return resolve(text, ROOT) if text else None

    def refresh(self) -> None:
        branch = projects.git_branch(ROOT)
        commit = projects.last_commit(ROOT)
        self.repo_text.setText("  ·  ".join(
            part for part in (display_path(ROOT, ROOT.parent), branch, commit)
            if part))
        docker = shutil.which("docker") is not None
        if self.container_switch.isChecked():
            self.container_hint.setText(
                "builds and flashes inside Docker (scripts/docker-dev.sh)"
                if docker else "docker not found in PATH")
            set_tone(self.container_hint, "" if docker else "warning")
        else:
            missing = [tool for tool in ("openocd", "cmake")
                       if shutil.which(tool) is None]
            self.container_hint.setText(
                "host tools: " + (f"{', '.join(missing)} missing" if missing
                                  else "openocd and cmake"))
            set_tone(self.container_hint, "warning" if missing else "")
        self.refresh_firmware()

    def refresh_firmware(self) -> None:
        path = self.firmware()
        if path is None:
            self.firmware_hint.setText("Choose the BOOT firmware to flash")
            set_tone(self.firmware_hint, "")
            self.set_pill("No firmware", "")
            self._nav_status("", False, "no firmware selected")
            return
        if not path.is_file():
            self.firmware_hint.setText("Not built yet · Build creates it")
            set_tone(self.firmware_hint, "")
            self.set_pill("Not built", "")
            self._nav_status("", False, "not built")
            return
        if path.suffix.lower() not in BOOT_SUFFIXES:
            self.firmware_hint.setText("Must be a .elf, .hex or .bin file")
            set_tone(self.firmware_hint, "danger")
            self.set_pill("Wrong file", "danger")
            return
        size = human_size(path.stat().st_size)
        note = f" · loads at {BOOT_ADDRESS}" if path.suffix == ".bin" else ""
        self.firmware_hint.setText(f"{path.name} · {size} · {stamp(path)}{note}")
        set_tone(self.firmware_hint, "")
        self.set_pill("Built", "success")
        self._nav_status("success", True, f"{path.name} built")

    def _nav_status(self, tone: str, filled: bool, tip: str) -> None:
        if self.nav is not None:
            self.nav.set_status(tone, filled, tip)

    def browse(self) -> None:
        path = self.firmware()
        start = path.parent if path and path.parent.is_dir() else ROOT / "build"
        name, _ = QFileDialog.getOpenFileName(
            self, "Choose BOOT firmware", str(start if start.is_dir() else ROOT),
            "Firmware (*.elf *.hex *.bin);;All files (*)")
        if name:
            self.firmware_edit.setText(display_path(Path(name)))

    # ---- jobs -----------------------------------------------------------

    def jobs(self, steps: list[str]) -> tuple[list[Job], list[str]]:
        jobs: list[Job] = []
        container = self.container_switch.isChecked()
        firmware = self.firmware()
        if "build" in steps:
            command = self.build_edit.text().strip() or "make build"
            if not container and command == "make build":
                command = ("cmake -S . -B build -G Ninja "
                           "-DCMAKE_BUILD_TYPE=Release && cmake --build build")
            jobs.append(Job("build", "bash", ["-lc", command], "Building BOOT…",
                            ROOT, command))
        if "flash" in steps:
            if firmware is None:
                raise RuntimeError("Choose the BOOT firmware to flash.")
            if "build" not in steps and not firmware.is_file():
                raise RuntimeError(f"{display_path(firmware)} does not exist. "
                                   "Build the bootloader first.")
            if firmware.suffix.lower() not in BOOT_SUFFIXES:
                raise RuntimeError("BOOT firmware must be a .elf, .hex or "
                                   ".bin file.")
            jobs.append(self._flash_job(firmware, container))
        return jobs, steps

    def _flash_job(self, firmware: Path, container: bool) -> Job:
        if container:
            args = [str(DEV_SCRIPT), "flash", str(firmware)]
            if firmware.suffix.lower() == ".bin":
                args.append(BOOT_ADDRESS)
            return Job("flash", "bash", args, "Flashing BOOT over SWD…", ROOT,
                       "docker-dev.sh " + " ".join(args[1:]))
        target = f'"{firmware}"'
        if firmware.suffix.lower() == ".bin":
            target += f" {BOOT_ADDRESS}"
        args = ["-f", self.host.openocd_cfg(), "-c",
                f"program {target} verify reset exit"]
        return Job("flash", "openocd", args, "Flashing BOOT over SWD…", ROOT)

    def _swd(self, action: str, text: str) -> None:
        """probe / reset / erase through the container or host tools."""
        if self.host.busy():
            return
        if self.container_switch.isChecked():
            job = Job(action, "bash", [str(DEV_SCRIPT), action], text, ROOT,
                      f"docker-dev.sh {action}")
        elif action == "probe":
            job = Job(action, "st-info", ["--probe"], text, ROOT)
        else:
            command = {"reset": "init; reset run; exit",
                       "erase": "init; reset halt; stm32f2x mass_erase 0; exit"}
            job = Job(action, "openocd", ["-f", self.host.openocd_cfg(), "-c",
                                          command[action]], text, ROOT)
        self.host.run(self, [job], [])

    def probe(self) -> None:
        self._swd("probe", "Probing the ST-LINK…")

    def reset_board(self) -> None:
        self._swd("reset", "Resetting the board…")

    def erase(self) -> None:
        if self.host.confirm(
                "Erase the entire flash?",
                "BOOT, both application slots and the factory identity "
                "sector are erased. The board does nothing until you flash "
                "BOOT again.", "Erase", destructive=True):
            self._swd("erase", "Erasing the flash…")

    def provision(self) -> None:
        firmware = self.firmware()
        if firmware is None or not firmware.is_file():
            self.host.error("Provision board", "Build or choose the BOOT "
                            "firmware first; provisioning writes it.")
            return
        if not self.host.confirm(
                "Provision this board?",
                "Sets dual-bank flash and software watchdogs in the option "
                "bytes, mass-erases the whole 2 MB flash and writes BOOT. "
                "Needed once for a new board.", "Provision", destructive=True):
            return
        if not self.host.busy():
            self.host.run(self, [Job("provision", "bash",
                                     [str(PROVISION), str(firmware)],
                                     "Provisioning the board…", ROOT,
                                     f"provision.sh {display_path(firmware)}")],
                          [])

    # ---- settings -------------------------------------------------------

    def load(self, settings: QSettings) -> None:
        self.build_edit.setText(settings.value("boot/build", "make build",
                                               type=str) or "make build")
        saved = settings.value("boot/firmware", "", type=str)
        self.firmware_edit.setText(saved or display_path(self.default_firmware))
        self.container_switch.setChecked(settings.value(
            "boot/container", self.container_switch.isChecked(), type=bool))

    def save(self, settings: QSettings) -> None:
        settings.setValue("boot/build", self.build_edit.text().strip())
        settings.setValue("boot/firmware", self.firmware_edit.text().strip())
        settings.setValue("boot/container", self.container_switch.isChecked())


# ---------------------------------------------------------------------------


class FirmwarePage(Page):
    uses_serial = True
    STEPS = (("clone", "Clone"), ("build", "Build"), ("sign", "Sign"),
             ("flash", "Flash"))

    def __init__(self, host, project: projects.FirmwareProject) -> None:
        self.project = project
        self.key = project.name
        self.title = project.title or project.name
        self.subtitle = project.summary
        self.icon_name = project.icon
        self.nav_subtitle = project.name
        super().__init__(host)
        self._key_cache: tuple[tuple[str, float] | None, str | None] = (None, None)

        # Source -----------------------------------------------------------
        self.clone_button = QPushButton("Clone")
        self.clone_button.clicked.connect(lambda: self.run_steps(["clone"]))
        source = Card("Source")
        self.repo_text = ElidedLabel(name="Mono")
        self.repo_hint = ElidedLabel()
        source.add_row("Repository", field(self.repo_text, self.repo_hint),
                       self.clone_button, stretch=0, align_top=True)

        self.build_edit = QLineEdit()
        self.build_edit.setPlaceholderText("run in the repository")
        self.image_edit = QLineEdit()
        self.image_edit.setPlaceholderText("relative to the repository")
        self.image_edit.textChanged.connect(self.refresh_image)
        self.image_hint = ElidedLabel()
        source.add_row("Build command", self.build_edit, stretch=0)
        source.add_row("Image", field(self.image_edit, self.image_hint),
                       stretch=0, align_top=True)
        self.add_card(source)

        # Signing ----------------------------------------------------------
        signing = Card("Signing")
        self.key_edit = QLineEdit()
        self.key_edit.setPlaceholderText("P-256 private key (.pem)")
        self.key_edit.textChanged.connect(self.refresh_signing)
        self.key_hint = ElidedLabel()
        self.key_button = QPushButton("Choose…")
        self.key_button.clicked.connect(self.browse_key)
        signing.add_row("Signing key", field(self.key_edit, self.key_hint),
                        self.key_button, stretch=0, align_top=True)

        self.counter_spin = QSpinBox()
        self.counter_spin.setRange(-1, 0x7FFFFFFF)
        self.counter_spin.setSpecialValueText("Match board")
        self.counter_spin.setFixedWidth(150)
        self.counter_spin.setToolTip(
            "Anti-rollback security counter (docs/IMAGE_SIGNING.md §8).")
        self.counter_spin.valueChanged.connect(self.refresh_signing)
        self.counter_hint = ElidedLabel()
        signing.add_row("Security counter", self.counter_spin,
                        self.counter_hint, stretch=1)

        self.activate_switch = Switch()
        self.activate_switch.setChecked(True)
        activate_hint = label("boots the new slot on trial and waits for the "
                              "application to confirm", "Hint")
        signing.add_row("Activate after flash", activate_hint, None,
                        self.activate_switch)
        self.add_card(signing)

        # Pipeline ---------------------------------------------------------
        menu = QMenu(self)
        menu.addAction("Pull latest changes", self.pull)
        menu.addAction("Open repository folder", self.open_folder)
        menu.addSeparator()
        menu.addAction("Enter bootloader", self.host.enter_boot)
        menu.addAction("Reset board", self.reset_board)
        self.add_pipeline(self._pipeline_card(
            "Build, Sign && Flash",
            [("Build", "build"), ("Sign", "sign"), ("Flash", "flash")], menu))
        self._set_icon(self.primary_button, "zap")
        self.finish_layout()

        self.inputs = [self.clone_button, self.build_edit, self.image_edit,
                       self.key_edit, self.key_button, self.counter_spin,
                       self.activate_switch]

    # ---- state ----------------------------------------------------------

    def repo(self) -> Path:
        return projects.repo_dir(self.project)

    def cloned(self) -> bool:
        return projects.is_cloned(self.project)

    def image(self) -> Path | None:
        text = self.image_edit.text().strip()
        return resolve(text, self.repo()) if text else None

    def signed(self) -> Path | None:
        image = self.image()
        return projects.signed_path(image) if image else None

    def sign_key(self) -> Path | None:
        text = self.key_edit.text().strip()
        return resolve(text, ROOT) if text else None

    def key_id(self) -> str | None:
        key = self.sign_key()
        if key is None or not key.is_file():
            return None
        try:
            ident = (str(key), key.stat().st_mtime)
        except OSError:
            return None
        if self._key_cache[0] != ident:
            self._key_cache = (ident, projects.key_id_of(key))
        return self._key_cache[1]

    def counter(self) -> int | None:
        """Counter to sign with; None keeps the header value."""
        value = self.counter_spin.value()
        return value if value >= 0 else self.host.board.security_floor

    def refresh(self) -> None:
        cloned = self.cloned()
        repo = self.repo()
        if cloned:
            branch = projects.git_branch(repo)
            self.repo_text.setText("  ·  ".join(
                part for part in (display_path(repo, repo.parent.parent),
                                  branch) if part))
            self.repo_hint.setText(projects.last_commit(repo) or "")
            set_tone(self.repo_hint, "")
        else:
            self.repo_text.setText(projects.clone_url(self.project))
            self.repo_hint.setText(f"Not cloned yet · clones into "
                                   f"{display_path(repo, repo.parent.parent)}")
            set_tone(self.repo_hint, "warning")
        self.clone_button.setVisible(not cloned)
        self.primary_button.setText("Clone, Build, Sign && Flash" if not cloned
                                    else "Build, Sign && Flash")
        if self.host.run_page is not self:
            self.stepper.reset({"clone"} if cloned else set())
            if cloned:
                self.stepper.set_note("clone", "cloned")
        self.refresh_image()
        self.refresh_signing()

    def refresh_image(self) -> None:
        image, signed = self.image(), self.signed()
        cloned = self.cloned()
        if not cloned:
            self.image_hint.setText("Available after cloning")
            self.set_pill("Not cloned", "warning")
            self._nav_status("", False, "not cloned")
            return
        if image is None:
            self.image_hint.setText("Enter the image the build produces")
            self.set_pill("", "")
            return
        parts = []
        if image.is_file():
            parts.append(f"{image.name} · {image_summary(image) or 'not a BOOT image'}"
                         f" · {stamp(image)}")
        else:
            parts.append("Not built yet")
        fresh_signed = (signed is not None and signed.is_file() and
                        (not image.is_file() or
                         signed.stat().st_mtime >= image.stat().st_mtime))
        if fresh_signed:
            parts.append(f"signed → {signed.name}")
        self.image_hint.setText("  ·  ".join(parts))
        if fresh_signed:
            self.set_pill("Signed", "success")
            self._nav_status("success", True, "built and signed")
        elif image.is_file():
            self.set_pill("Built", "accent")
            self._nav_status("accent", True, "built, not signed")
        else:
            self.set_pill("Not built", "")
            self._nav_status("", True, "cloned, not built")

    def _nav_status(self, tone: str, filled: bool, tip: str) -> None:
        if self.nav is not None:
            self.nav.set_status(tone, filled, tip)

    def refresh_signing(self, *_args) -> None:
        board = self.host.board
        key = self.sign_key()
        tone = ""
        if key is None:
            text, tone = "Choose the key BOOT trusts", "warning"
        elif not key.is_file():
            text, tone = "Key file not found", "danger"
        else:
            ident = self.key_id()
            if ident is None:
                text = "Key id unknown (install the cryptography package)"
                tone = "warning"
            elif board.key_id and ident != board.key_id:
                text = (f"Key {ident}, but the board trusts {board.key_id}: "
                        "BOOT will refuse the image")
                tone = "danger"
            else:
                text = f"Key {ident}" + (" · matches the board"
                                         if board.key_id else "")
                tone = "success" if board.key_id else ""
            if projects.is_dev_key(key):
                text += " · development key, bench only"
        self.key_hint.setText(text)
        set_tone(self.key_hint, tone)

        value = self.counter_spin.value()
        floor = board.security_floor
        tone = ""
        if value < 0:
            text = (f"Signs with {floor}, the board's floor" if floor is not None
                    else "Keeps the image's counter until a board is connected")
        elif floor is None:
            text = "Board floor unknown"
        elif value < floor:
            text, tone = f"Below the board floor {floor}: BOOT will refuse it", \
                "danger"
        elif value > floor:
            text, tone = (f"Raises the floor from {floor} for good once the "
                          "image confirms"), "warning"
        else:
            text = "Equal to the board floor"
        self.counter_hint.setText(text)
        set_tone(self.counter_hint, tone)

    def board_info_changed(self) -> None:
        self.refresh_signing()

    def browse_key(self) -> None:
        key = self.sign_key()
        start = key.parent if key and key.parent.is_dir() else ROOT / "keys"
        name, _ = QFileDialog.getOpenFileName(
            self, "Choose the signing key", str(start),
            "PEM key (*.pem);;All files (*)")
        if name:
            self.key_edit.setText(display_path(Path(name)))

    # ---- jobs -----------------------------------------------------------

    def run_all(self) -> None:
        steps = ["build", "sign", "flash"]
        if not self.cloned():
            steps.insert(0, "clone")
        self.run_steps(steps)

    def jobs(self, steps: list[str]) -> tuple[list[Job], list[str]]:
        project, repo = self.project, self.repo()
        cloned = self.cloned()
        if "clone" in steps and cloned:
            steps = [step for step in steps if step != "clone"]
        if not cloned and "clone" not in steps:
            raise RuntimeError(f"{project.name} is not cloned yet. Clone it "
                               "first.")
        if "clone" in steps and repo.exists() and any(repo.iterdir()):
            raise RuntimeError(f"{repo} exists but is not a firmware "
                               "repository. Move it away and clone again.")
        image, signed, key = self.image(), self.signed(), self.sign_key()
        if image is None and ({"sign", "flash"} & set(steps)):
            raise RuntimeError("Enter the image the build produces.")

        jobs: list[Job] = []
        if "clone" in steps:
            args = projects.clone_args(project)
            jobs.append(Job("clone", "git", args, f"Cloning {project.name}…",
                            repo.parent, "git " + " ".join(args),
                            env=GIT_ENV))
        if "build" in steps:
            command = self.build_edit.text().strip()
            if not command:
                raise RuntimeError("Enter the build command.")
            jobs.append(Job("build", "bash", ["-lc", command],
                            f"Building {project.name}…", repo,
                            f"{command}   ({project.name})"))
        if "sign" in steps:
            if key is None or not key.is_file():
                raise RuntimeError("Choose an existing signing key (.pem).")
            if "build" not in steps and not image.is_file():
                raise RuntimeError(f"{image.name} is not built yet. Build "
                                   "first.")
            port = self.host.port()
            if self.counter_spin.value() < 0 and \
                    self.host.board.security_floor is None and port:
                # "Match board" needs BOOT's floor; read it first (this
                # switches a running application to BOOT, as flashing would).
                info = [str(BOOTCTL), "--port", port, "--timeout", "3",
                        "info", "--json"]
                jobs.append(Job("sign", sys.executable, info,
                                "Reading the board's security floor…", ROOT,
                                "bootctl.py " + " ".join(info[1:]),
                                capture=True,
                                done=self._absorb_board))
            sign_job = Job("sign", sys.executable, [],
                           f"Signing {signed.name}…", ROOT)
            sign_job.resolve = lambda: self._sign_args(sign_job, image, key,
                                                       signed)
            jobs.append(sign_job)
            inspect = projects.inspect_args(signed, key)
            jobs.append(Job("sign", sys.executable, inspect,
                            f"Checking {signed.name}…", ROOT,
                            projects.display_command("mkimage.py",
                                                     inspect[1:])))
        if "flash" in steps:
            port = self.host.port()
            if not port:
                raise RuntimeError("Connect the board and choose its serial "
                                   "port in the sidebar.")
            if "sign" not in steps and not signed.is_file():
                raise RuntimeError(f"{signed.name} does not exist. Sign the "
                                   "image first.")
            if not self._confirm_flash():
                return [], steps
            command = ["--port", port, "--baud", self.host.baud(),
                       "--verbose", "update", str(signed), "--json"]
            if self.activate_switch.isChecked():
                command += ["--activate", "--wait"]
            slot = self.host.board.target_slot
            jobs.append(Job("flash", sys.executable, [str(BOOTCTL), *command],
                            f"Flashing {project.name}"
                            + (f" to slot {slot}…" if slot else "…"),
                            ROOT, "bootctl.py " + " ".join(command)))
        return jobs, steps

    def _absorb_board(self, text: str) -> None:
        if not self.host.absorb_board_json(text):
            raise RuntimeError("could not read the board's security floor")
        self.host.console_line(
            f"board floor {self.host.board.security_floor}, key "
            f"{self.host.board.key_id or 'unknown'}")

    def _sign_args(self, job: Job, image: Path, key: Path,
                   signed: Path) -> list[str]:
        """Evaluated when the job starts, after any board read."""
        args = projects.sign_args(image, key, signed, self.counter())
        job.display = projects.display_command("mkimage.py", args[1:])
        return args

    def _confirm_flash(self) -> bool:
        board = self.host.board
        problems = []
        ident = self.key_id()
        if board.key_id and ident and ident != board.key_id:
            problems.append(f"The image is signed with key {ident}, but the "
                            f"board trusts {board.key_id}. BOOT will refuse it.")
        counter = self.counter()
        if board.security_floor is not None and counter is not None:
            if counter < board.security_floor:
                problems.append(f"Security counter {counter} is below the "
                                f"board floor {board.security_floor}. BOOT will "
                                "refuse it.")
            elif counter > board.security_floor:
                problems.append(
                    f"Security counter {counter} raises the board floor from "
                    f"{board.security_floor} once the image confirms. Images "
                    "below it are then refused on this board for good.")
        if board.flash_mode and board.flash_mode != "DUAL_BANK":
            problems.append("The board is in single-bank flash mode. Provision "
                            "it from the Bootloader page first.")
        if not problems:
            return True
        return self.host.confirm("Flash anyway?", "\n\n".join(problems),
                                 "Flash", destructive=True)

    def pull(self) -> None:
        if not self.cloned():
            self.host.error(self.title, "Clone the repository first.")
            return
        if not self.host.busy():
            self.host.run(self, [Job("pull", "git", ["pull", "--ff-only"],
                                     f"Pulling {self.project.name}…",
                                     self.repo(), "git pull --ff-only",
                                     env=GIT_ENV)], [])

    def open_folder(self) -> None:
        target = self.repo() if self.cloned() else self.repo().parent
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(target)))

    def reset_board(self) -> None:
        self.host.serial_command("reset", "Resetting the board…")

    def run_finished(self, failed: bool) -> None:
        self.refresh_image()
        self.refresh_signing()
        cloned = self.cloned()
        self.clone_button.setVisible(not cloned)
        self.primary_button.setText("Clone, Build, Sign && Flash" if not cloned
                                    else "Build, Sign && Flash")
        if cloned and self.stepper.states.get("clone") == "pending":
            self.stepper.set_state("clone", "done", "cloned")

    # ---- settings -------------------------------------------------------

    def load(self, settings: QSettings) -> None:
        base = f"fw/{self.project.name}"
        self.build_edit.setText(settings.value(f"{base}/build", "", type=str)
                                or self.project.build)
        self.image_edit.setText(settings.value(f"{base}/image", "", type=str)
                                or self.project.image)
        self.key_edit.setText(settings.value(f"{base}/key", "", type=str) or
                              display_path(projects.default_signing_key()))
        self.counter_spin.setValue(settings.value(f"{base}/counter", -1,
                                                  type=int))
        self.activate_switch.setChecked(settings.value(f"{base}/activate",
                                                       True, type=bool))

    def save(self, settings: QSettings) -> None:
        base = f"fw/{self.project.name}"
        settings.setValue(f"{base}/build", self.build_edit.text().strip())
        settings.setValue(f"{base}/image", self.image_edit.text().strip())
        settings.setValue(f"{base}/key", self.key_edit.text().strip())
        settings.setValue(f"{base}/counter", self.counter_spin.value())
        settings.setValue(f"{base}/activate", self.activate_switch.isChecked())
