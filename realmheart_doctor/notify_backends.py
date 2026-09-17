"""Desktop notification backends: best-effort delivery, never health-critical."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess

_BACKEND_TIMEOUT = 3


def _notify_send(title: str, body: str) -> None:
    binary = shutil.which("notify-send")
    if binary is None:
        raise RuntimeError("notify-send unavailable")
    subprocess.run([binary, "-a", "Realmheart Doctor", title, body],
                   timeout=_BACKEND_TIMEOUT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _desktop_backend():
    if os.environ.get("REALMHEART_DOCTOR_NOTIFY_BACKEND") == "none":
        return None
    if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        return _notify_send
    return None


def deliver(title: str, body: str) -> bool:
    """Deliver one actionable notification; False keeps health logic intact."""
    backend = _desktop_backend()
    if backend is None:
        return False
    try:
        backend(title, body)
    except Exception:
        return False
    return True
