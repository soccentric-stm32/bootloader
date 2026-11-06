#!/usr/bin/env python3
"""
Tests for the flash GUI's firmware project model (tools/gui/projects.py)
and the signing fields of its boot info view (tools/gui/device.py).

Neither module needs PySide6, so these run wherever the other Python tests
do.  BSAFE-006: the GUI signs with mkimage.py --sign before uploading.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

from gui import projects
from gui.device import parse_boot_info

DEV_KEY_ID = "0xfaa7171b"


def test_five_firmware_projects_with_documented_images():
    images = {project.name: project.image for project in projects.PROJECTS}
    assert images == {
        "app-c-freertos": "build/demo.img",
        "app-c-zephyr": "build/zephyr-app.img",
        "app-rust-freertos": "build/rust-app.img",
        "app-rust-zephyr": "build/rust-zephyr-app.img",
        "validation-firmware": "build/hwval.img",
    }
    assert projects.find_project("validation-firmware").build == "make build"
    assert projects.find_project("nope") is None


def test_workspace_override(monkeypatch, tmp_path):
    monkeypatch.setenv("FIRMWARE_WORKSPACE", str(tmp_path))
    project = projects.find_project("app-c-zephyr")
    assert projects.repo_dir(project) == tmp_path / "app-c-zephyr"
    assert not projects.is_cloned(project)
    (tmp_path / "app-c-zephyr").mkdir()
    (tmp_path / "app-c-zephyr" / "Makefile").write_text("build:\n")
    assert projects.is_cloned(project)


def test_git_branch(tmp_path):
    (tmp_path / ".git").mkdir()
    (tmp_path / ".git" / "HEAD").write_text("ref: refs/heads/develop\n")
    assert projects.git_branch(tmp_path) == "develop"
    (tmp_path / ".git" / "HEAD").write_text("0123456789abcdef\n")
    assert projects.git_branch(tmp_path) == "01234567"
    assert projects.git_branch(tmp_path / "missing") == ""


def test_signed_path_sits_next_to_the_image():
    image = pathlib.Path("/w/app/build/demo.img")
    assert projects.signed_path(image) == \
        pathlib.Path("/w/app/build/demo-signed.img")


def test_dev_key_id_and_detection(tmp_path):
    assert projects.key_id_of(projects.DEV_SIGNING_KEY) == DEV_KEY_ID
    assert projects.is_dev_key(projects.DEV_SIGNING_KEY)
    assert projects.key_id_of(tmp_path / "missing.pem") is None


def test_sign_args_keep_or_set_counter(tmp_path):
    image, key, out = tmp_path / "a.img", tmp_path / "k.pem", tmp_path / "o"
    keep = projects.sign_args(image, key, out, None)
    assert "--security-counter" not in keep
    assert keep[-2:] == ["--output", str(out)]
    set_ = projects.sign_args(image, key, out, 2)
    assert set_[set_.index("--security-counter") + 1] == "2"


def test_sign_and_inspect_commands_produce_a_valid_image(tmp_path):
    binary = tmp_path / "app.bin"
    # Minimal vector table: initial SP in SRAM, Thumb reset handler in slot.
    binary.write_bytes((0x20080000).to_bytes(4, "little") +
                       (0x0800C101).to_bytes(4, "little") + bytes(248))
    image = tmp_path / "app.img"
    subprocess.run([sys.executable, str(projects.MKIMAGE), "--input",
                    str(binary), "--output", str(image)], check=True)
    signed = projects.signed_path(image)
    key = projects.DEV_SIGNING_KEY
    subprocess.run([sys.executable,
                    *projects.sign_args(image, key, signed, 3)], check=True)
    result = subprocess.run(
        [sys.executable, *projects.inspect_args(signed, key)],
        check=True, capture_output=True, text=True)
    assert "signature     : valid" in result.stdout
    assert "security ctr  : 3" in result.stdout


def test_boot_info_signing_fields():
    view = parse_boot_info({"active_slot": "a", "target_slot": "B",
                            "signed_images": True, "key_id": "0xFAA7171B",
                            "security_floor": 2, "slots": []})
    assert view.signed_images
    assert view.key_id == DEV_KEY_ID
    assert view.security_floor == 2
    old = parse_boot_info({"active_slot": "A"})
    assert old.security_floor is None
    assert old.key_id == ""


def test_clone_url_from_workspace_repos_yaml(monkeypatch, tmp_path):
    monkeypatch.setenv("FIRMWARE_WORKSPACE", str(tmp_path))
    project = projects.find_project("app-rust-zephyr")
    assert projects.clone_url(project) == \
        "git@github.com:soccentric-stm32/app-rust-zephyr.git"
    (tmp_path / "repos.yaml").write_text(
        "repos:\n"
        "  - name: app-c-zephyr\n    url: https://example.com/wrong.git\n"
        "  - name: app-rust-zephyr\n"
        "    url: \"https://example.com/rust-zephyr.git\"\n")
    assert projects.clone_url(project) == "https://example.com/rust-zephyr.git"
    assert projects.clone_args(project) == [
        "clone", "--", "https://example.com/rust-zephyr.git",
        str(tmp_path / "app-rust-zephyr")]


def test_runner_resolves_args_and_captures_output():
    from gui.runner import Job, Runner
    from PySide6.QtCore import QCoreApplication, QEventLoop, QTimer

    app = QCoreApplication.instance() or QCoreApplication([])
    runner = Runner()
    lines, captured, finished = [], [], []
    runner.output.connect(lines.append)
    runner.allFinished.connect(finished.append)
    first = Job("read", sys.executable, ["-c", "print('{\"floor\": 2}')"],
                "read", capture=True, done=captured.append)
    second = Job("sign", sys.executable, [], "sign")
    second.resolve = lambda: ["-c", f"print('counter', {len(captured)})"]
    runner.start([first, second])
    loop = QEventLoop()
    runner.allFinished.connect(lambda _failed: loop.quit())
    QTimer.singleShot(10000, loop.quit)
    if not finished:
        loop.exec()
    assert finished == [False]
    assert captured == ['{"floor": 2}']
    assert "counter 1" in lines              # args computed after job 1
    assert "{\"floor\": 2}" not in lines      # kept out of the log
    del app
