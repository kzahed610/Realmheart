"""Append-only, fsync-backed write-ahead journal."""

from __future__ import annotations

import json
import os
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from ..constants import JOURNAL_SCHEMA_VERSION
from ..durability import fsync_directory
from ..errors import JournalCorruptError
from ..models import OperationState, to_jsonable


@dataclass(frozen=True)
class JournalEvent:
    schema_version: int
    seq: int
    time: str
    operation_id: str
    state: OperationState
    kind: str | None = None
    target: str | None = None
    data: dict[str, Any] | None = None

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "JournalEvent":
        try:
            return cls(
                schema_version=int(payload["schema_version"]),
                seq=int(payload["seq"]),
                time=str(payload["time"]),
                operation_id=str(payload["operation_id"]),
                state=OperationState(payload["state"]),
                kind=payload.get("kind"),
                target=payload.get("target"),
                data=payload.get("data"),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise JournalCorruptError(f"Invalid journal event: {exc}") from exc


class WriteAheadJournal:
    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self._lock = threading.Lock()
        events = read_journal(self.path)
        self._next_seq = events[-1].seq + 1 if events else 1

    def append(
        self,
        *,
        operation_id: str,
        state: OperationState,
        kind: str | None = None,
        target: str | None = None,
        data: dict[str, Any] | None = None,
    ) -> JournalEvent:
        with self._lock:
            event = JournalEvent(
                schema_version=JOURNAL_SCHEMA_VERSION,
                seq=self._next_seq,
                time=datetime.now(timezone.utc).isoformat(),
                operation_id=operation_id,
                state=state,
                kind=kind,
                target=target,
                data=to_jsonable(data) if data is not None else None,
            )
            encoded = json.dumps(to_jsonable(event), sort_keys=True, separators=(",", ":")) + "\n"
            with self.path.open("a", encoding="utf-8") as handle:
                handle.write(encoded)
                handle.flush()
                os.fsync(handle.fileno())
            fsync_directory(self.path.parent)
            self._next_seq += 1
            return event


def read_journal(path: Path) -> list[JournalEvent]:
    """Read complete JSONL records, tolerating one truncated final line only."""

    path = Path(path)
    if not path.exists():
        return []

    raw = path.read_bytes()
    if not raw:
        return []

    complete_terminated = raw.endswith(b"\n")
    lines = raw.splitlines()
    events: list[JournalEvent] = []

    expected_seq = 1
    for index, raw_line in enumerate(lines, start=1):
        if not raw_line.strip():
            continue
        is_last = index == len(lines)
        try:
            payload = json.loads(raw_line)
        except json.JSONDecodeError as exc:
            if is_last and not complete_terminated:
                break
            raise JournalCorruptError(
                f"Malformed complete JSONL record: {exc}", line_number=index
            ) from exc

        event = JournalEvent.from_dict(payload)
        if event.seq != expected_seq:
            raise JournalCorruptError(
                f"Journal sequence discontinuity: expected {expected_seq}, got {event.seq}",
                line_number=index,
            )
        expected_seq += 1
        events.append(event)

    return events


def iter_operation_events(events: Iterable[JournalEvent], operation_id: str) -> Iterable[JournalEvent]:
    return (event for event in events if event.operation_id == operation_id)
