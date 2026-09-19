"""Notification backends: Event Surface first, desktop fallback, never critical.

Doctor incident notifications belong in Realmheart's own notification surface,
so the default order is:

1. Event Surface (`realmheart-event send`), which the shell renders in its
   notification UI and history, and which needs no graphical environment;
2. desktop `notify-send`, when a session is available at all;
3. nothing — delivery failure never breaks health logic.

``REALMHEART_DOCTOR_NOTIFY_BACKEND`` accepts ``event``, ``desktop``, ``none``;
``auto`` (the default) prefers the Event Surface.  ``REALMHEART_EVENT_BIN``
overrides the binary lookup explicitly.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess

_BACKEND_TIMEOUT = 3
_EVENT_SOURCE = "realmheart-doctor"


def _event_binary() -> str | None:
    override = os.environ.get("REALMHEART_EVENT_BIN")
    if override:
        return override
    return shutil.which("realmheart-event")


def _event_surface(title: str, body: str, severity: str) -> None:
    binary = _event_binary()
    if binary is None:
        raise RuntimeError("realmheart-event unavailable")
    event_id = "realmheart-doctor-" + hashlib.sha256(
        body.encode("utf-8", "surrogateescape")
    ).hexdigest()[:16]
    summary = " ".join(body.split())
    argv = (
        binary, "send", "--id", event_id, "--source", _EVENT_SOURCE,
        "--title", title, "--summary", summary[:400],
        "--severity", severity, "--presentation", "attention",
    )
    subprocess.run(argv, timeout=_BACKEND_TIMEOUT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _notify_send(title: str, body: str) -> None:
    binary = shutil.which("notify-send")
    if binary is None:
        raise RuntimeError("notify-send unavailable")
    subprocess.run([binary, "-a", "Realmheart Doctor", title, body],
                   timeout=_BACKEND_TIMEOUT, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _desktop_backend():
    if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        return _notify_send
    return None


def _resolve_backend(choice: str):
    if choice == "none":
        return None
    if choice == "event":
        return _event_surface
    if choice == "desktop":
        return _desktop_backend()
    if _event_binary() is not None:
        return _event_surface
    return _desktop_backend()


def deliver(title: str, body: str, *, severity: str = "warning") -> bool:
    """Deliver one actionable notification; False keeps health logic intact."""
    raw = (os.environ.get("REALMHEART_DOCTOR_NOTIFY_BACKEND") or "auto").strip().lower()
    choice = raw if raw in {"auto", "event", "desktop", "none"} else "auto"
    backend = _resolve_backend(choice)
    if backend is None:
        return False
    try:
        if backend is _event_surface:
            backend(title, body, severity)
        else:
            backend(title, body)
    except Exception:
        return False
    return True
