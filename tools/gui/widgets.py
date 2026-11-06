"""Custom widgets for the flash GUI.

Everything painted here reads the current theme tokens at paint time, so a
light/dark switch only needs update().  Every Qt name used inside a
paintEvent is imported at the top: an exception there aborts the process.
"""

from __future__ import annotations

from typing import ClassVar

from PySide6.QtCore import (
    Property,
    QEasingCurve,
    QPointF,
    QPropertyAnimation,
    QRectF,
    QSize,
    Qt,
    QTimer,
)
from PySide6.QtGui import QColor, QFont, QFontMetrics, QPainter, QPen
from PySide6.QtWidgets import (
    QAbstractButton,
    QFrame,
    QHBoxLayout,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from . import icons, theme

# Fits the longest row label ("Activate after flash") with room to spare.
ROW_LABEL_WIDTH = 152


def _mix(a: QColor, b: QColor, t: float) -> QColor:
    return QColor(int(a.red() + (b.red() - a.red()) * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue() + (b.blue() - a.blue()) * t))


def repolish(widget: QWidget) -> None:
    widget.style().unpolish(widget)
    widget.style().polish(widget)


def set_tone(label: QLabel, tone: str) -> None:
    """Pill/Hint colour: '', accent, success, warning or danger."""
    if label.property("tone") != tone:
        label.setProperty("tone", tone)
        repolish(label)


def label(text: str, name: str, parent: QWidget | None = None) -> QLabel:
    widget = QLabel(text, parent)
    widget.setObjectName(name)
    return widget


def hairline() -> QFrame:
    line = QFrame()
    line.setObjectName("Hairline")
    line.setFixedHeight(1)
    return line


class ElidedLabel(QLabel):
    """Single-line label that elides in the middle and shows a tooltip."""

    def __init__(self, text: str = "", name: str = "Hint",
                 mode: Qt.TextElideMode = Qt.ElideMiddle) -> None:
        super().__init__()
        self.setObjectName(name)
        self._full = ""
        self._mode = mode
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        self.setMinimumWidth(40)
        self.setText(text)

    def setText(self, text: str) -> None:
        self._full = text
        self.setToolTip(text if text else "")
        self._elide()

    def fullText(self) -> str:
        return self._full

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._elide()

    def _elide(self) -> None:
        metrics = QFontMetrics(self.font())
        super().setText(metrics.elidedText(self._full, self._mode,
                                           max(self.width(), 40)))


class Card(QFrame):
    """Grouped surface: optional title row, then rows split by hairlines."""

    def __init__(self, title: str = "", trailing: QWidget | None = None) -> None:
        super().__init__()
        self.setObjectName("Card")
        self._box = QVBoxLayout(self)
        self._box.setContentsMargins(0, 0, 0, 0)
        self._box.setSpacing(0)
        self._rows = 0
        if title:
            head = QHBoxLayout()
            head.setContentsMargins(16, 12, 12, 8)
            head.setSpacing(8)
            head.addWidget(label(title, "CardTitle"))
            head.addStretch(1)
            if trailing is not None:
                head.addWidget(trailing)
            self._box.addLayout(head)

    def add_row(self, title: str | None, *items, stretch: int = -1,
                align_top: bool = False) -> QWidget:
        """One row: a fixed-width label, then widgets (None = stretch)."""
        if self._rows:
            self._box.addWidget(hairline())
        self._rows += 1
        row = QWidget()
        layout = QHBoxLayout(row)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(12)
        if title is not None:
            name = label(title, "RowLabel")
            name.setFixedWidth(ROW_LABEL_WIDTH)
            layout.addWidget(name, 0, Qt.AlignTop if align_top else
                             Qt.AlignVCenter)
        for index, item in enumerate(items):
            if item is None:
                layout.addStretch(1)
            elif isinstance(item, QWidget):
                layout.addWidget(item, 1 if index == stretch else 0)
            else:
                layout.addLayout(item, 1 if index == stretch else 0)
        self._box.addWidget(row)
        return row

    def add_widget(self, widget: QWidget) -> None:
        if self._rows:
            self._box.addWidget(hairline())
        self._rows += 1
        self._box.addWidget(widget)


def field(*widgets: QWidget, spacing: int = 4) -> QVBoxLayout:
    """Stack an input over its hint."""
    column = QVBoxLayout()
    column.setContentsMargins(0, 0, 0, 0)
    column.setSpacing(spacing)
    for widget in widgets:
        column.addWidget(widget)
    return column


class Switch(QAbstractButton):
    """macOS-style on/off switch."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setCheckable(True)
        self.setCursor(Qt.PointingHandCursor)
        self.setFocusPolicy(Qt.StrongFocus)
        self._offset = 0.0
        self._anim = QPropertyAnimation(self, b"offset", self)
        self._anim.setDuration(160)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)
        self.toggled.connect(self._animate)

    def sizeHint(self) -> QSize:
        return QSize(42, 24)

    def setChecked(self, checked: bool) -> None:
        super().setChecked(checked)
        self._anim.stop()
        self._offset = 1.0 if checked else 0.0
        self.update()

    def _animate(self, checked: bool) -> None:
        self._anim.stop()
        self._anim.setStartValue(self._offset)
        self._anim.setEndValue(1.0 if checked else 0.0)
        self._anim.start()

    def _get_offset(self) -> float:
        return self._offset

    def _set_offset(self, value: float) -> None:
        self._offset = value
        self.update()

    offset = Property(float, _get_offset, _set_offset)

    def paintEvent(self, _event) -> None:
        t = theme.current()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        if not self.isEnabled():
            painter.setOpacity(0.4)
        track = QRectF(1, 1, 40, 22)
        off = QColor(t.border)
        on = QColor(t.accent)
        painter.setPen(Qt.NoPen)
        painter.setBrush(_mix(off, on, self._offset))
        painter.drawRoundedRect(track, 11, 11)
        if self.hasFocus():
            painter.setPen(QPen(QColor(t.accent), 2))
            painter.setBrush(Qt.NoBrush)
            painter.drawRoundedRect(track.adjusted(-1, -1, 1, 1), 12, 12)
            painter.setPen(Qt.NoPen)
        painter.setBrush(QColor("#ffffff"))
        x = 3 + self._offset * 18
        painter.drawEllipse(QRectF(x, 3, 18, 18))


class NavItem(QAbstractButton):
    """Sidebar entry: icon, title, subtitle and a status dot."""

    TONES: ClassVar[dict[str, str]] = {"": "text_tertiary", "success": "success", "warning": "warning",
             "accent": "accent", "danger": "danger"}

    def __init__(self, icon_name: str, title: str, subtitle: str) -> None:
        super().__init__()
        self.setCheckable(True)
        self.setAutoExclusive(True)
        self.setCursor(Qt.PointingHandCursor)
        self.setFocusPolicy(Qt.TabFocus)
        self.setAttribute(Qt.WA_Hover)
        self.icon_name = icon_name
        self.title = title
        self.subtitle = subtitle
        self.status_tone = ""
        self.status_filled = False
        self.setToolTip(f"{title} · {subtitle}")

    def set_status(self, tone: str, filled: bool, tip: str = "") -> None:
        self.status_tone = tone
        self.status_filled = filled
        self.setToolTip(f"{self.title} · {self.subtitle}" +
                        (f"\n{tip}" if tip else ""))
        self.update()

    def sizeHint(self) -> QSize:
        return QSize(200, 48)

    def paintEvent(self, _event) -> None:
        t = theme.current()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        rect = QRectF(self.rect())
        checked = self.isChecked()
        if checked:
            painter.setPen(Qt.NoPen)
            painter.setBrush(QColor(t.accent))
            painter.drawRoundedRect(rect, 8, 8)
        elif self.underMouse():
            painter.setPen(Qt.NoPen)
            painter.setBrush(QColor(0, 0, 0, 14) if not t.dark
                             else QColor(255, 255, 255, 18))
            painter.drawRoundedRect(rect, 8, 8)
        if self.hasFocus() and not checked:
            painter.setPen(QPen(QColor(t.accent), 2))
            painter.setBrush(Qt.NoBrush)
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 7, 7)

        fg = t.on_accent if checked else t.text
        secondary = QColor(t.on_accent if checked else t.text_secondary)
        if checked:
            secondary.setAlphaF(0.82)
        icons.paint(painter, self.icon_name,
                    QRectF(12, (rect.height() - 18) / 2, 18, 18),
                    t.on_accent if checked else t.text_secondary, 1.8)

        text_left = 42
        dot_space = 22
        width = rect.width() - text_left - dot_space
        font = QFont(self.font())
        font.setPixelSize(13)
        font.setWeight(QFont.DemiBold if checked else QFont.Medium)
        painter.setFont(font)
        painter.setPen(QColor(fg))
        metrics = QFontMetrics(font)
        painter.drawText(QRectF(text_left, 7, width, 18),
                         Qt.AlignLeft | Qt.AlignVCenter,
                         metrics.elidedText(self.title, Qt.ElideRight,
                                            int(width)))
        small = QFont(self.font())
        small.setPixelSize(11)
        painter.setFont(small)
        painter.setPen(secondary)
        painter.drawText(QRectF(text_left, 25, width, 16),
                         Qt.AlignLeft | Qt.AlignVCenter,
                         QFontMetrics(small).elidedText(
                             self.subtitle, Qt.ElideRight, int(width)))

        dot = QColor(getattr(t, self.TONES.get(self.status_tone,
                                                "text_tertiary")))
        if checked:
            dot = QColor(t.on_accent)
        centre = QPointF(rect.width() - 14, rect.height() / 2)
        if self.status_filled:
            painter.setPen(Qt.NoPen)
            painter.setBrush(dot)
        else:
            painter.setPen(QPen(dot, 1.5))
            painter.setBrush(Qt.NoBrush)
        painter.drawEllipse(centre, 3.5, 3.5)


class StepTracker(QWidget):
    """Horizontal pipeline: numbered circles joined by lines.

    States per step: pending, running, done, failed, skipped.  A running
    step spins and may show a percentage.
    """

    def __init__(self, steps) -> None:
        super().__init__()
        self.steps = steps
        self.states = {key: "pending" for key, _ in steps}
        self.notes = {key: "" for key, _ in steps}
        self._angle = 0
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._spin)
        self.setMinimumHeight(68)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

    def sizeHint(self) -> QSize:
        return QSize(120 * len(self.steps), 68)

    def set_state(self, key: str, state: str, note: str = "") -> None:
        if key not in self.states:
            return
        self.states[key] = state
        self.notes[key] = note
        running = any(value == "running" for value in self.states.values())
        if running and not self._timer.isActive():
            self._timer.start()
        elif not running:
            self._timer.stop()
        self.update()

    def set_note(self, key: str, note: str) -> None:
        if key in self.notes and self.notes[key] != note:
            self.notes[key] = note
            self.update()

    def reset(self, done: set[str] | None = None) -> None:
        for key in self.states:
            self.states[key] = "done" if done and key in done else "pending"
            self.notes[key] = ""
        self._timer.stop()
        self.update()

    def _spin(self) -> None:
        self._angle = (self._angle + 8) % 360
        self.update()

    def paintEvent(self, _event) -> None:
        t = theme.current()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        count = len(self.steps)
        if not count:
            return
        width = self.width()
        slot = width / count
        radius = 12.0
        cy = 16.0
        centres = [QPointF(slot * i + slot / 2, cy) for i in range(count)]

        for i in range(count - 1):
            left = self.states[self.steps[i][0]]
            colour = QColor(t.success if left in ("done", "skipped")
                            else t.hairline)
            painter.setPen(QPen(colour, 2))
            painter.drawLine(QPointF(centres[i].x() + radius + 6, cy),
                             QPointF(centres[i + 1].x() - radius - 6, cy))

        number_font = QFont(self.font())
        number_font.setPixelSize(12)
        number_font.setWeight(QFont.DemiBold)
        label_font = QFont(self.font())
        label_font.setPixelSize(12)
        note_font = QFont(self.font())
        note_font.setPixelSize(11)

        for index, ((key, text), centre) in enumerate(zip(self.steps, centres)):
            state = self.states[key]
            circle = QRectF(centre.x() - radius, centre.y() - radius,
                            radius * 2, radius * 2)
            if state == "done":
                painter.setPen(Qt.NoPen)
                painter.setBrush(QColor(t.success))
                painter.drawEllipse(circle)
                icons.paint(painter, "check", circle.adjusted(6, 6, -6, -6),
                            "#ffffff", 3)
            elif state == "failed":
                painter.setPen(Qt.NoPen)
                painter.setBrush(QColor(t.danger))
                painter.drawEllipse(circle)
                icons.paint(painter, "x", circle.adjusted(7, 7, -7, -7),
                            "#ffffff", 3)
            elif state == "running":
                painter.setPen(QPen(QColor(t.accent_soft), 2.5))
                painter.setBrush(Qt.NoBrush)
                painter.drawEllipse(circle)
                pen = QPen(QColor(t.accent), 2.5)
                pen.setCapStyle(Qt.RoundCap)
                painter.setPen(pen)
                painter.drawArc(circle, -self._angle * 16, 100 * 16)
                painter.setPen(QColor(t.accent))
                painter.setFont(number_font)
                painter.drawText(circle, Qt.AlignCenter, str(index + 1))
            else:
                skipped = state == "skipped"
                painter.setPen(QPen(QColor(t.border), 1.5,
                                    Qt.DashLine if skipped else Qt.SolidLine))
                painter.setBrush(QColor(t.surface))
                painter.drawEllipse(circle)
                painter.setPen(QColor(t.text_tertiary))
                painter.setFont(number_font)
                painter.drawText(circle, Qt.AlignCenter,
                                 "–" if skipped else str(index + 1))

            painter.setFont(label_font)
            painter.setPen(QColor(
                t.accent if state == "running" else
                t.danger if state == "failed" else
                t.text if state == "done" else t.text_secondary))
            painter.drawText(QRectF(centre.x() - slot / 2, cy + radius + 4,
                                    slot, 18), Qt.AlignCenter, text)
            note = self.notes.get(key, "")
            if note:
                painter.setFont(note_font)
                painter.setPen(QColor(t.text_tertiary))
                metrics = QFontMetrics(note_font)
                painter.drawText(
                    QRectF(centre.x() - slot / 2, cy + radius + 21, slot, 16),
                    Qt.AlignCenter,
                    metrics.elidedText(note, Qt.ElideRight, int(slot - 8)))
