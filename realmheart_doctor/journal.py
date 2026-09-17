"""Doctor's internal journal: bounded JSON lines, local-only, never secrets."""
from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
from tempfile import NamedTemporaryFile

_LOG_NAME = "doctor.log"
_MAX_DEFAULT_BYTES = 512 * 1024
_KEEP_TAIL_DEFAULT = 400
_SECRET_HINTS = ("password", "token", "secret", "authorization", "api_key", "passwd")
_MAX_FIELD = 400


def journal(state_root: Path, event: str, *, max_bytes: int = _MAX_DEFAULT_BYTES,
            keep_tail: int = _KEEP_TAIL_DEFAULT, **fields: object) -> None:
    """Append one bounded event; a broken journal never breaks the caller."""
    try:
        root = Path(state_root)
        root.mkdir(parents=True, exist_ok=True)
        path = root / _LOG_NAME
        entry = {"timestamp": datetime.now(timezone.utc).isoformat(), "event": event}
        for key, value in fields.items():
            if value is None:
                continue
            text = str(value)
            if any(hint in key.lower() for hint in _SECRET_HINTS):
                text = "[REDACTED]"
            from .redaction import sanitize_text

            text = sanitize_text(text, username="", hostname="", home="")
            entry[key] = text[:_MAX_FIELD]
        line = json.dumps(entry, sort_keys=True) + "\n"
        try:
            if path.stat().st_size > max_bytes:
                _rotate(path, keep_tail=keep_tail)
        except FileNotFoundError:
            pass
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC, 0o600)
        with os.fdopen(descriptor, "a", encoding="utf-8") as stream:
            stream.write(line)
    except OSError:
        return


def _rotate(path: Path, *, keep_tail: int) -> None:
    try:
        with path.open("r", encoding="utf-8") as existing:
            lines = existing.readlines()
    except OSError:
        return
    rotated = path.with_name(_LOG_NAME + ".1")
    os.replace(path, rotated)
    tail = lines[-keep_tail:]
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_CLOEXEC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.writelines(tail)


def read_journal(state_root: Path, *, limit: int = 200) -> list[dict]:
    """Return the most recent journal events for local debugging."""
    try:
        lines = (Path(state_root) / _LOG_NAME).read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    events: list[dict] = []
    for line in lines[-limit:]:
        try:
            payload = json.loads(line)
        except ValueError:
            continue
        if isinstance(payload, dict):
            events.append(payload)
    return events
