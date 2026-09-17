"""Read-only pacman log correlation: bounded windows, evidence not proof.

Parsing is deliberately narrow: only ``[ALPM] upgraded <pkg> (a -> b)`` lines
inside the requested window are surfaced.  A matching transaction is correlation
evidence for an incident; it never proves causation by itself.
"""
from __future__ import annotations

import re
from datetime import datetime, timezone

from .state import STATE_FORMAT_VERSION

_TRANSACTION = re.compile(
    r"^\[(?P<stamp>[^\]]+)\]\s+\[ALPM\]\s+"
    r"(?P<action>upgraded|downgraded|reinstalled)\s+"
    r"(?P<package>\S+)\s+\((?P<previous>\S+)\s+->\s+(?P<current>\S+)\)\s*$"
)


def _stamp_epoch(stamp: str) -> float | None:
    try:
        parsed = datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%S%z")
    except ValueError:
        return None
    return parsed.timestamp()


def correlate_package_changes(
    log_text: str,
    relevant_packages: set[str] | frozenset[str],
    *,
    window: tuple[int, int] | None = None,
) -> list[dict[str, object]]:
    """Extract upgrade transactions for relevant packages inside a window.

    ``window`` bounds analysis to ``(start_epoch, end_epoch)``; ``None`` means
    no time filtering.  Every result carries ``proves_causation: False`` — a
    package change in the window is evidence, not proof of a regression.
    """

    changes: list[dict[str, object]] = []
    for line in log_text.splitlines():
        match = _TRANSACTION.match(line.strip())
        if match is None:
            continue
        package = match.group("package")
        if package not in relevant_packages:
            continue
        timestamp = _stamp_epoch(match.group("stamp"))
        if window is not None:
            if timestamp is None or not window[0] <= timestamp <= window[1]:
                continue
        changes.append({
            "format_version": STATE_FORMAT_VERSION,
            "type": "PACKAGE_TRANSACTION",
            "action": match.group("action"),
            "package": package,
            "previous": match.group("previous"),
            "current": match.group("current"),
            "timestamp": match.group("stamp"),
            "proves_causation": False,
        })
    return changes
