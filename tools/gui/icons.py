"""Stroke icons for the flash GUI, rendered from inline SVG.

One style everywhere: 24x24 grid, 2 px round strokes, no fills.  Icons are
recoloured per state and theme at render time, so there is a single source
per glyph.
"""

from __future__ import annotations

import atexit
import shutil
import tempfile
from pathlib import Path

from PySide6.QtCore import QByteArray, QRectF, QSize, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

_GLYPHS = {
    # chip: the bootloader
    "chip": '<rect x="5" y="5" width="14" height="14" rx="2"/>'
            '<rect x="9" y="9" width="6" height="6" rx="1"/>'
            '<path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/>',
    # app window: the applications
    "app": '<rect x="3" y="4" width="18" height="16" rx="2"/>'
           '<path d="M3 9h18M7 6.5h.01M10 6.5h.01"/>',
    # gear-ish cog for Rust projects
    "cog": '<circle cx="12" cy="12" r="3"/>'
           '<path d="M12 2v3M12 19v3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M2 12h3'
           'M19 12h3M4.9 19.1L7 17M17 7l2.1-2.1"/>',
    # flask: validation firmware
    "flask": '<path d="M9 3h6M10 3v6L4.5 18.5A1.7 1.7 0 0 0 6 21h12a1.7 1.7 0 0 0'
             ' 1.5-2.5L14 9V3"/><path d="M7 15h10"/>',
    "zap": '<path d="M13 2L4 14h7l-1 8 9-12h-7l1-8z"/>',
    "hammer": '<path d="M15 12l-8.5 8.5a2.1 2.1 0 0 1-3-3L12 9"/>'
              '<path d="M17.6 15L22 10.6M20.9 11.7l-6.6-6.6a4 4 0 0 0-5.7 0'
              'L8 5.7l7.3 7.3"/>',
    "key": '<circle cx="7.5" cy="15.5" r="4.5"/>'
           '<path d="M10.7 12.3L21 2M16 7l3 3M19 4l2 2"/>',
    "download": '<path d="M12 3v12M7 10l5 5 5-5M5 21h14"/>',
    "branch": '<circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/>'
              '<circle cx="18" cy="8" r="2.5"/>'
              '<path d="M6 8.5v7M18 10.5a6 6 0 0 1-6 6H8.5"/>',
    "refresh": '<path d="M21 12a9 9 0 1 1-2.6-6.4L21 8"/><path d="M21 3v5h-5"/>',
    "more": '<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/>'
            '<circle cx="19" cy="12" r="1"/>',
    "folder": '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2'
              'H5a2 2 0 0 1-2-2z"/>',
    "plug": '<path d="M9 2v5M15 2v5M6 7h12v4a6 6 0 0 1-12 0zM12 17v5"/>',
    "check": '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
    "x": '<path d="M6 6l12 12M18 6L6 18"/>',
    "alert": '<path d="M12 3L2 20h20z"/><path d="M12 10v4M12 17h.01"/>',
    "stop": '<rect x="6" y="6" width="12" height="12" rx="2"/>',
    "reset": '<path d="M3 12a9 9 0 1 0 2.6-6.4L3 8"/><path d="M3 3v5h5"/>',
    "trash": '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
    "search": '<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>',
    "login": '<path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/>'
             '<path d="M10 17l5-5-5-5M15 12H3"/>',
    "info": '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5h.01"/>',
    "shield": '<path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z"/>'
              '<path d="M9 12l2 2 4-4"/>',
    "terminal": '<rect x="3" y="4" width="18" height="16" rx="2"/>'
                '<path d="M7 9l3 3-3 3M12 15h5"/>',
    "chevron-down": '<path d="M6 9l6 6 6-6"/>',
    "chevron-up": '<path d="M6 15l6-6 6 6"/>',
    "copy": '<rect x="8" y="8" width="13" height="13" rx="2"/>'
            '<path d="M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3"/>',
    "book": '<path d="M4 4.5A2.5 2.5 0 0 1 6.5 2H20v18H6.5A2.5 2.5 0 0 0 4 22.5z"/>'
            '<path d="M4 20V4.5"/>',
}


def svg(name: str, color: str, stroke: float = 2.0) -> str:
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
            f'fill="none" stroke="{color}" stroke-width="{stroke}" '
            'stroke-linecap="round" stroke-linejoin="round">'
            f'{_GLYPHS[name]}</svg>')


def pixmap(name: str, color: str, size: int = 16, ratio: float = 2.0) -> QPixmap:
    renderer = QSvgRenderer(QByteArray(svg(name, color).encode()))
    pix = QPixmap(QSize(int(size * ratio), int(size * ratio)))
    pix.fill(Qt.transparent)
    painter = QPainter(pix)
    painter.setRenderHint(QPainter.Antialiasing)
    renderer.render(painter, QRectF(0, 0, size * ratio, size * ratio))
    painter.end()
    pix.setDevicePixelRatio(ratio)
    return pix


def icon(name: str, color: str, disabled: str | None = None,
         size: int = 16) -> QIcon:
    result = QIcon()
    result.addPixmap(pixmap(name, color, size), QIcon.Normal)
    if disabled:
        result.addPixmap(pixmap(name, disabled, size), QIcon.Disabled)
    return result


def paint(painter: QPainter, name: str, rect: QRectF, color: str,
          stroke: float = 2.0) -> None:
    """Draw a glyph straight into a custom paintEvent."""
    QSvgRenderer(QByteArray(svg(name, color, stroke).encode())).render(
        painter, rect)


_ASSET_DIR = Path(tempfile.mkdtemp(prefix="boot-flasher-"))
atexit.register(shutil.rmtree, _ASSET_DIR, ignore_errors=True)


def asset(name: str, color: str) -> str:
    """Write a glyph to a file for QSS url() rules; returns a posix path."""
    path = _ASSET_DIR / f"{name}-{color.lstrip('#')}.svg"
    if not path.exists():
        path.write_text(svg(name, color, 2.4), encoding="utf-8")
    return path.as_posix()
