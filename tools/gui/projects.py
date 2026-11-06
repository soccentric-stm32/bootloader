"""Firmware projects the flash GUI can build, sign and flash.

Every application image BOOT accepts must be signed (BSAFE-006).  The
application builds run in their own containers without the Python
`cryptography` package, so the GUI signs on the host after the build with
tools/mkimage.py --sign, then uploads the signed image through BOOT.
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MKIMAGE = ROOT / "tools" / "mkimage.py"
DEV_SIGNING_KEY = ROOT / "keys" / "dev-signing-key.pem"


ORG_URL = "git@github.com:soccentric-stm32/{name}.git"


@dataclass(frozen=True)
class FirmwareProject:
    name: str           # repository directory in the workspace
    label: str          # short description for the project list
    image: str          # unsigned image the build produces, repo-relative
    title: str = ""     # display name
    summary: str = ""   # one sentence for the page header
    icon: str = "app"   # glyph in tools/gui/icons.py
    build: str = "make build"


# Images per application: docs/IMAGE_SIGNING.md section 7.
PROJECTS: tuple[FirmwareProject, ...] = (
    FirmwareProject("app-c-freertos", "C on FreeRTOS", "build/demo.img",
                    "C FreeRTOS", "Demo application in C on FreeRTOS.",
                    "app"),
    FirmwareProject("app-c-zephyr", "C on Zephyr", "build/zephyr-app.img",
                    "C Zephyr", "Demo application in C on Zephyr RTOS.",
                    "app"),
    FirmwareProject("app-rust-freertos", "Rust on Embassy",
                    "build/rust-app.img", "Rust Embassy",
                    "Demo application in Rust on the Embassy executor.",
                    "cog"),
    FirmwareProject("app-rust-zephyr", "Rust on Zephyr",
                    "build/rust-zephyr-app.img", "Rust Zephyr",
                    "Demo application in Rust on Zephyr RTOS.", "cog"),
    FirmwareProject("validation-firmware", "HWVAL validation",
                    "build/hwval.img", "Validation firmware",
                    "HWVAL hardware validation firmware.", "flask"),
)


def find_project(name: str) -> FirmwareProject | None:
    for project in PROJECTS:
        if project.name == name:
            return project
    return None


def workspace_dir() -> Path:
    """Directory holding the cloned repositories (FIRMWARE_WORKSPACE)."""
    return Path(os.environ.get("FIRMWARE_WORKSPACE", ROOT.parent)).expanduser()


def repo_dir(project: FirmwareProject) -> Path:
    return workspace_dir() / project.name


def is_cloned(project: FirmwareProject) -> bool:
    return (repo_dir(project) / "Makefile").is_file()


def repos_file() -> Path:
    return workspace_dir() / "repos.yaml"


def clone_url(project: FirmwareProject) -> str:
    """URL from the workspace repos.yaml, else the organisation default."""
    try:
        text = repos_file().read_text(encoding="utf-8")
    except OSError:
        text = ""
    name = None
    for line in text.splitlines():
        match = re.match(r"\s*-\s*name:\s*['\"]?([^'\"\s]+)", line)
        if match:
            name = match.group(1)
            continue
        match = re.match(r"\s+url:\s*['\"]?([^'\"\s]+)", line)
        if match and name == project.name:
            return match.group(1)
    return ORG_URL.format(name=project.name)


def clone_args(project: FirmwareProject) -> list[str]:
    return ["clone", "--", clone_url(project), str(repo_dir(project))]


def last_commit(repo: Path) -> str:
    """'abc1234 Subject line' for HEAD, or '' when git is unavailable."""
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), "log", "-1", "--format=%h %s"],
            capture_output=True, text=True, timeout=2, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def git_branch(repo: Path) -> str:
    """Current branch from .git/HEAD without running git; '' if unknown."""
    try:
        head = (repo / ".git" / "HEAD").read_text(encoding="utf-8").strip()
    except OSError:
        return ""
    prefix = "ref: refs/heads/"
    if head.startswith(prefix):
        return head[len(prefix):]
    return head[:8]


def default_signing_key() -> Path:
    return Path(os.environ.get("BOOT_SIGNING_KEY", DEV_SIGNING_KEY))


def is_dev_key(path: Path) -> bool:
    try:
        return path.resolve() == DEV_SIGNING_KEY.resolve()
    except OSError:
        return False


def signed_path(image: Path) -> Path:
    """build/demo.img -> build/demo-signed.img, next to the input."""
    return image.with_name(f"{image.stem}-signed{image.suffix}")


def key_id_of(path: Path) -> str | None:
    """'0x1234abcd' for a private or public PEM, or None if unreadable.

    Needs `cryptography`; the GUI then cannot warn about a key mismatch
    before uploading, but BOOT still refuses the image.
    """
    try:
        if str(MKIMAGE.parent) not in sys.path:
            sys.path.insert(0, str(MKIMAGE.parent))
        import mkimage
        point = mkimage.public_point(mkimage.load_key(path))
        return f"0x{mkimage.key_id(point):08x}"
    except Exception:  # noqa: BLE001 - missing package, bad file, ...
        return None


def sign_args(image: Path, key: Path, output: Path,
              counter: int | None) -> list[str]:
    args = [str(MKIMAGE), "--sign", str(image), "--key", str(key)]
    if counter is not None:
        args += ["--security-counter", str(counter)]
    return args + ["--output", str(output)]


def inspect_args(image: Path, key: Path) -> list[str]:
    return [str(MKIMAGE), "--inspect", str(image), "--key", str(key)]


def display_command(program: str, args: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in [program, *args])
