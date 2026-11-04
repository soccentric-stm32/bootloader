"""Device model helpers for the flash GUI (P3-6).

The GUI no longer guesses the target slot or scans /dev/tty* itself: BOOT's
`bootctl --json` output is normalised here and port discovery is shared with
the command-line tool.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class SlotView:
    slot: str = "?"
    present: bool = False
    valid: bool = False
    pending: bool = False
    confirmed: bool = False
    rejected: bool = False
    sequence: int = 0
    trial_count: int = 0
    version: str = ""
    detail: str = ""


@dataclass
class BootInfoView:
    active_slot: str | None = None
    active_valid: bool = False
    target_slot: str | None = None
    flash_mode: str = ""
    boot_version: str = ""
    boot_build: str = ""
    last_outcome: str = ""
    slots: list[SlotView] = field(default_factory=list)

    @property
    def target(self) -> str | None:
        return self.target_slot


def _slot(value) -> str | None:
    text = str(value or "").upper()
    return text if text in ("A", "B") else None


def parse_boot_info(data: dict | None) -> BootInfoView | None:
    if data is None:
        return None
    view = BootInfoView(
        active_slot=_slot(data.get("active_slot")),
        active_valid=bool(data.get("active_valid", False)),
        target_slot=_slot(data.get("target_slot")),
        flash_mode=str(data.get("flash_mode", "")),
        boot_version=str(data.get("boot_version", "")),
        boot_build=str(data.get("boot_build", "")),
        last_outcome=str(data.get("last_outcome", "")),
    )
    for item in data.get("slots", []):
        view.slots.append(SlotView(
            slot=_slot(item.get("slot")) or "?",
            present=bool(item.get("present")),
            valid=bool(item.get("valid")),
            pending=bool(item.get("pending")),
            confirmed=bool(item.get("confirmed")),
            rejected=bool(item.get("rejected")),
            sequence=int(item.get("sequence", 0)),
            trial_count=int(item.get("trial_count", 0)),
            version=str(item.get("version", "")),
            detail=str(item.get("detail", "")),
        ))
    return view


def stlink_ports() -> list[str]:
    """ST-LINK VCP ports (VID 0x0483), shared with bootctl.py."""
    try:
        from bootctl import list_stlink_ports
        return list_stlink_ports()
    except Exception:  # noqa: BLE001 - pyserial may be missing
        import glob
        return sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
