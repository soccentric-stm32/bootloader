"""Sequential QProcess queue for the production workflow (P3-6)."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from PySide6.QtCore import QObject, QProcess, Signal


@dataclass
class Job:
    step: str
    program: str
    args: list[str]
    text: str
    cwd: Path | None = None
    display: str | None = None


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
        self.process.setWorkingDirectory(
            str(job.cwd) if job.cwd is not None else str(Path.cwd()))
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
            self.output.emit(line.rstrip("\r"))

    def _flush(self) -> None:
        if self._buffer:
            self.output.emit(self._buffer.rstrip("\r\n"))
            self._buffer = ""

    def _on_finished(self, code: int, status: QProcess.ExitStatus) -> None:
        self._drain()
        self._flush()
        job = self.current
        self.current = None
        success = status == QProcess.NormalExit and code == 0
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
