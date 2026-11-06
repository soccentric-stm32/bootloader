"""Colour tokens and the one application style sheet for the flash GUI.

Light and dark follow the system colour scheme.  Every rule is scoped by
object name so nothing leaks onto child widgets (QLabel is a QFrame).
"""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication


@dataclass(frozen=True)
class Tokens:
    dark: bool
    window: str
    sidebar: str
    surface: str
    surface_alt: str
    border: str
    hairline: str
    text: str
    text_secondary: str
    text_tertiary: str
    accent: str
    accent_hover: str
    accent_pressed: str
    accent_soft: str
    on_accent: str
    success: str
    success_soft: str
    warning: str
    warning_soft: str
    danger: str
    danger_soft: str
    hover: str
    pressed: str
    selection: str
    console: str
    console_text: str


LIGHT = Tokens(
    dark=False,
    window="#f5f5f7", sidebar="#ececef", surface="#ffffff",
    surface_alt="#f9f9fb", border="#d1d1d6", hairline="#e5e5ea",
    text="#1d1d1f", text_secondary="#6e6e73", text_tertiary="#8e8e93",
    accent="#007aff", accent_hover="#0071e3", accent_pressed="#0062cc",
    accent_soft="#e5f1ff", on_accent="#ffffff",
    success="#248a3d", success_soft="#e3f6e8",
    warning="#b25000", warning_soft="#fff2e0",
    danger="#d70015", danger_soft="#ffe9e8",
    hover="rgba(0, 0, 0, 0.05)", pressed="rgba(0, 0, 0, 0.10)",
    selection="#b3d7ff", console="#fbfbfd", console_text="#1d1d1f",
)

DARK = Tokens(
    dark=True,
    window="#1e1e20", sidebar="#28282b", surface="#2c2c2e",
    surface_alt="#323235", border="#48484a", hairline="#3a3a3c",
    text="#f5f5f7", text_secondary="#a1a1a6", text_tertiary="#7c7c80",
    accent="#0a84ff", accent_hover="#409cff", accent_pressed="#0070e0",
    accent_soft="#0a2a4d", on_accent="#ffffff",
    success="#30d158", success_soft="#11331c",
    warning="#ff9f0a", warning_soft="#3d2a0a",
    danger="#ff453a", danger_soft="#401714",
    hover="rgba(255, 255, 255, 0.06)", pressed="rgba(255, 255, 255, 0.12)",
    selection="#0a4a8f", console="#1a1a1c", console_text="#e5e5ea",
)

_current = LIGHT


def current() -> Tokens:
    return _current


def system_is_dark() -> bool:
    app = QGuiApplication.instance()
    if app is None:
        return False
    return app.styleHints().colorScheme() == Qt.ColorScheme.Dark


def set_dark(dark: bool) -> Tokens:
    global _current
    _current = DARK if dark else LIGHT
    return _current


def color(name: str) -> QColor:
    return QColor(getattr(_current, name))


def _family(candidates: list[str], fallback: str) -> str:
    families = set(QFontDatabase.families())
    for name in candidates:
        if name in families:
            return name
    return fallback


def ui_font() -> QFont:
    family = _family(
        ["SF Pro Text", "Helvetica Neue", "Segoe UI", "Inter", "Ubuntu",
         "Cantarell", "Noto Sans", "DejaVu Sans"], "Sans Serif")
    font = QFont(family)
    font.setPixelSize(13)
    return font


def mono_font() -> QFont:
    family = _family(
        ["SF Mono", "Menlo", "JetBrains Mono", "Cascadia Mono", "Consolas",
         "Ubuntu Mono", "DejaVu Sans Mono"], "Monospace")
    font = QFont(family)
    font.setPixelSize(12)
    font.setStyleHint(QFont.Monospace)
    return font


def stylesheet(t: Tokens) -> str:
    return f"""
QMainWindow, QWidget#Root {{ background: {t.window}; color: {t.text}; }}
QWidget#Content, QWidget#Page, QScrollArea#PageScroll,
QWidget#PageScrollBody {{ background: {t.window}; border: none; }}
QWidget#Sidebar {{ background: {t.sidebar}; border-right: 1px solid {t.hairline}; }}
QLabel {{ background: transparent; color: {t.text}; }}

QLabel#AppTitle {{ font-size: 15px; font-weight: 600; }}
QLabel#AppSubtitle, QLabel#Caption {{ font-size: 12px; color: {t.text_secondary}; }}
QLabel#SidebarSection {{ font-size: 11px; font-weight: 600; color: {t.text_tertiary}; }}
QLabel#PageTitle {{ font-size: 22px; font-weight: 600; }}
QLabel#PageSubtitle {{ font-size: 13px; color: {t.text_secondary}; }}
QLabel#CardTitle {{ font-size: 13px; font-weight: 600; }}
QLabel#RowLabel {{ font-size: 13px; color: {t.text}; }}
QLabel#Hint {{ font-size: 12px; color: {t.text_secondary}; }}
QLabel#Hint[tone="warning"] {{ color: {t.warning}; }}
QLabel#Hint[tone="danger"] {{ color: {t.danger}; }}
QLabel#Hint[tone="success"] {{ color: {t.success}; }}
QLabel#Mono {{ font-size: 12px; color: {t.text_secondary}; }}

QLabel#Pill {{ border-radius: 10px; padding: 3px 10px; font-size: 12px;
              font-weight: 600; background: {t.hover}; color: {t.text_secondary}; }}
QLabel#Pill[tone="accent"] {{ background: {t.accent_soft}; color: {t.accent}; }}
QLabel#Pill[tone="success"] {{ background: {t.success_soft}; color: {t.success}; }}
QLabel#Pill[tone="warning"] {{ background: {t.warning_soft}; color: {t.warning}; }}
QLabel#Pill[tone="danger"] {{ background: {t.danger_soft}; color: {t.danger}; }}

QFrame#Card {{ background: {t.surface}; border: 1px solid {t.hairline};
              border-radius: 10px; }}
QFrame#Hairline {{ background: {t.hairline}; border: none; }}
QFrame#DeviceBox {{ background: {t.surface}; border: 1px solid {t.hairline};
                   border-radius: 10px; }}

QLineEdit, QComboBox, QSpinBox {{
    background: {t.surface}; color: {t.text}; border: 1px solid {t.border};
    border-radius: 6px; padding: 0 10px; min-height: 30px; font-size: 13px;
    selection-background-color: {t.selection}; selection-color: {t.text}; }}
QLineEdit:hover, QComboBox:hover, QSpinBox:hover {{ border-color: {t.text_tertiary}; }}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus {{ border: 1px solid {t.accent}; }}
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {{
    color: {t.text_tertiary}; background: {t.surface_alt}; border-color: {t.hairline}; }}
QComboBox {{ padding-right: 28px; }}
QComboBox::drop-down {{ subcontrol-origin: padding; subcontrol-position: center right;
                       width: 24px; border: none; background: transparent; }}
QComboBox::down-arrow {{ image: url(__CHEVRON__); width: 12px; height: 12px; }}
QComboBox QAbstractItemView {{ background: {t.surface}; color: {t.text};
    border: 1px solid {t.border}; border-radius: 6px; padding: 4px; outline: none;
    selection-background-color: {t.accent}; selection-color: {t.on_accent}; }}
QSpinBox {{ padding-right: 24px; }}
QSpinBox::up-button, QSpinBox::down-button {{ subcontrol-origin: border;
    width: 22px; border: none; background: transparent; }}
QSpinBox::up-button {{ subcontrol-position: top right; }}
QSpinBox::down-button {{ subcontrol-position: bottom right; }}
QSpinBox::up-button:hover, QSpinBox::down-button:hover {{ background: {t.hover}; }}
QSpinBox::up-arrow {{ image: url(__CHEVRON_UP__); width: 10px; height: 10px; }}
QSpinBox::down-arrow {{ image: url(__CHEVRON__); width: 10px; height: 10px; }}

QPushButton {{ background: {t.surface}; color: {t.text}; border: 1px solid {t.border};
              border-radius: 7px; padding: 0 14px; min-height: 32px;
              font-size: 13px; font-weight: 500; }}
QPushButton:hover {{ background: {t.surface_alt}; border-color: {t.text_tertiary}; }}
QPushButton:pressed {{ background: {t.hover}; }}
QPushButton:focus {{ border: 1px solid {t.accent}; }}
QPushButton:disabled {{ color: {t.text_tertiary}; background: {t.surface_alt};
                       border-color: {t.hairline}; }}
QPushButton::menu-indicator {{ image: none; width: 0; }}
QPushButton#Primary {{ background: {t.accent}; color: {t.on_accent};
    border: 1px solid {t.accent}; min-height: 40px; padding: 0 20px;
    font-size: 14px; font-weight: 600; }}
QPushButton#Primary:hover {{ background: {t.accent_hover}; border-color: {t.accent_hover}; }}
QPushButton#Primary:pressed {{ background: {t.accent_pressed}; }}
QPushButton#Primary:focus {{ border: 2px solid {t.accent_soft}; }}
QPushButton#Primary:disabled {{ background: {t.accent}; border-color: {t.accent};
    color: rgba(255, 255, 255, 0.55); }}
QPushButton#Secondary {{ min-height: 40px; padding: 0 16px; }}
QPushButton#Danger {{ color: {t.danger}; }}
QPushButton#Danger:hover {{ background: {t.danger_soft}; border-color: {t.danger}; }}
QPushButton#Link, QPushButton#IconButton {{ background: transparent; border: none;
    color: {t.accent}; padding: 0 6px; min-height: 26px; }}
QPushButton#Link:hover {{ color: {t.accent_hover}; }}
QPushButton#IconButton {{ min-width: 28px; max-width: 28px; padding: 0;
                         border-radius: 6px; }}
QPushButton#IconButton:hover {{ background: {t.hover}; }}
QPushButton#IconButton:pressed {{ background: {t.pressed}; }}
QPushButton#Link:disabled, QPushButton#IconButton:disabled {{ color: {t.text_tertiary}; }}

QPlainTextEdit#Console {{ background: {t.console}; color: {t.console_text};
    border: none; border-top: 1px solid {t.hairline};
    border-bottom-left-radius: 10px; border-bottom-right-radius: 10px;
    padding: 8px 12px; selection-background-color: {t.selection}; }}

QSplitter::handle {{ background: transparent; }}

QProgressBar#Progress {{ background: {t.hairline}; border: none; border-radius: 2px;
                        max-height: 4px; min-height: 4px; }}
QProgressBar#Progress::chunk {{ background: {t.accent}; border-radius: 2px; }}

QMenuBar {{ background: {t.window}; color: {t.text}; }}
QMenuBar::item {{ padding: 4px 10px; background: transparent; border-radius: 4px; }}
QMenuBar::item:selected {{ background: {t.hover}; }}
QMenu {{ background: {t.surface}; color: {t.text}; border: 1px solid {t.border};
        border-radius: 8px; padding: 4px; }}
QMenu::item {{ padding: 6px 24px 6px 12px; border-radius: 5px; }}
QMenu::item:selected {{ background: {t.accent}; color: {t.on_accent}; }}
QMenu::item:disabled {{ color: {t.text_tertiary}; }}
QMenu::separator {{ height: 1px; background: {t.hairline}; margin: 4px 8px; }}
QMenu::indicator {{ width: 14px; height: 14px; }}

QToolTip {{ background: {t.surface}; color: {t.text}; border: 1px solid {t.border};
           padding: 4px 8px; border-radius: 6px; }}

QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
QScrollBar::handle:vertical {{ background: {t.border}; border-radius: 4px;
                              min-height: 24px; }}
QScrollBar::handle:vertical:hover {{ background: {t.text_tertiary}; }}
QScrollBar:horizontal {{ background: transparent; height: 10px; margin: 2px; }}
QScrollBar::handle:horizontal {{ background: {t.border}; border-radius: 4px;
                                min-width: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; width: 0; }}
QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}

QMessageBox {{ background: {t.surface}; }}
QMessageBox QLabel {{ color: {t.text}; font-size: 13px; }}
"""
