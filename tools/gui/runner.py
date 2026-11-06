"""Sequential QProcess queue: clone, build, sign and flash jobs run in order."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, QProcessEnvironment, Signal


@dataclass
class Job:
    step: str
    program: str
    args: list[str]
    text: str
    cwd: Path | None = None
    display: str | None = None
    env: dict[str, str] | None = None   # added to the inherited environment
    # Computes args when the job starts, from results of earlier jobs.
    resolve: Callable[[], list[str]] | None = None
    # Keep the output out of the console and hand it to done() on success.
    capture: bool = False
    done: Callable[[str], None] | None = None


class Runner(QObject):
    """Run jobs one after another and keep the UI informed."""

    output = Signal(str)
    started = Signal(str, str)          # step, human text
    finished = Signal(str, bool, int)   # step, success, exit code
    allFinished = Signal(bool)          # any failure?

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.jobs: list[Job] = []
        self.current: Job | None = None
        self.failed = False
        self._buffer = ""
        self._captured: list[str] = []

        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.MergedChannels)
        self.process.readyReadStandardOutput.connect(self._drain)
        self.process.finished.connect(self._on_finished)
        self.process.errorOccurred.connect(self._on_error)

    def busy(self) -> bool:
        return self.process.state() != QProcess.NotRunning

    def start(self, jobs: list[Job]) -> None:
        if self.busy():
            return
        self.jobs = list(jobs)
        self.failed = False
        self._start_next()

    def cancel(self) -> None:
        self.jobs.clear()
        if self.busy():
            self.process.kill()

    def _start_next(self) -> None:
        if self.busy() or not self.jobs:
            if not self.jobs and not self.failed:
                self.allFinished.emit(False)
            return
        job = self.jobs.pop(0)
        self.current = job
        self._captured = []
        if job.resolve is not None:
            try:
                job.args = job.resolve()
            except Exception as error:  # noqa: BLE001 - report, don't crash
                self.output.emit(f"error: {error}")
                self.current = None
                self.failed = True
                self.jobs.clear()
                self.finished.emit(job.step, False, -1)
                self.allFinished.emit(True)
                return
        self.process.setWorkingDirectory(
            str(job.cwd) if job.cwd is not None else str(Path.cwd()))
        env = QProcessEnvironment.systemEnvironment()
        for key, value in (job.env or {}).items():
            env.insert(key, value)
        self.process.setProcessEnvironment(env)
        self.output.emit("$ " + (job.display or
                                 " ".join([job.program, *job.args])))
        self.started.emit(job.step, job.text)
        self.process.start(job.program, job.args)

    def _drain(self) -> None:
        text = bytes(self.process.readAllStandardOutput()).decode(
            "utf-8", "replace")
        self._buffer += text
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            self._line(line.rstrip("\r"))

    def _flush(self) -> None:
        if self._buffer:
            self._line(self._buffer.rstrip("\r\n"))
            self._buffer = ""

    def _line(self, line: str) -> None:
        if self.current is not None and self.current.capture:
            self._captured.append(line)
        else:
            self.output.emit(line)

    def _on_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        self._drain()
        self._flush()
        job = self.current
        self.current = None
        success = status == QProcess.NormalExit and code == 0
        if job is not None and job.capture and not success:
            for line in self._captured:
                self.output.emit(line)
        if job is not None and success and job.done is not None:
            try:
                job.done("\n".join(self._captured))
            except Exception as error:  # noqa: BLE001 - report, don't crash
                self.output.emit(f"error: {error}")
                success = False
        if job is not None:
            self.finished.emit(job.step, success, code)
        if not success:
            self.failed = True
            self.jobs.clear()
            self.allFinished.emit(True)
            return
        if self.jobs:
            self._start_next()
        else:
            self.allFinished.emit(False)

    def _on_error(self, error: QProcess.ProcessError) -> None:
        if error != QProcess.FailedToStart:
            return
        job = self.current
        self.current = None
        self.failed = True
        self.jobs.clear()
        if job is not None:
            self.finished.emit(job.step, False, -1)
        self.allFinished.emit(True)
