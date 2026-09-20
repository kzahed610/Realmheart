"""Read-only pacman log correlation: bounded windows, evidence not proof.

Pacman records version transitions (upgrade/downgrade) differently from
single-version transactions (install/remove/reinstall).  Doctor preserves that
shape instead of ignoring the transactions that matter most to diagnosis.
"""
from __future__ import annotations

import re
from datetime import datetime

from .state import STATE_FORMAT_VERSION

_VERSION_TRANSITION = re.compile(
    r"^\[(?P<stamp>[^\]]+)\]\s+\[ALPM\]\s+"
    r"(?P<action>upgraded|downgraded)\s+"
    r"(?P<package>\S+)\s+\((?P<previous>\S+)\s+->\s+(?P<current>\S+)\)\s*$"
)
_SINGLE_VERSION_TRANSACTION = re.compile(
    r"^\[(?P<stamp>[^\]]+)\]\s+\[ALPM\]\s+"
    r"(?P<action>installed|removed|reinstalled)\s+"
    r"(?P<package>\S+)\s+\((?P<version>\S+)\)\s*$"
)


def _stamp_epoch(stamp: str) -> float | None:
    try:
        parsed = datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%S%z")
    except ValueError:
        return None
    return parsed.timestamp()


def _parse_transaction(line: str) -> dict[str, str | None] | None:
    transition = _VERSION_TRANSITION.match(line)
    if transition is not None:
        return {
            "stamp": transition.group("stamp"),
            "action": transition.group("action"),
            "package": transition.group("package"),
            "previous": transition.group("previous"),
            "current": transition.group("current"),
        }
    single = _SINGLE_VERSION_TRANSACTION.match(line)
    if single is None:
        return None
    action = single.group("action")
    version = single.group("version")
    if action == "installed":
        previous, current = None, version
    elif action == "removed":
        previous, current = version, None
    else:  # reinstalled
        previous = current = version
    return {
        "stamp": single.group("stamp"),
        "action": action,
        "package": single.group("package"),
        "previous": previous,
        "current": current,
    }


def correlate_package_changes(
    log_text: str,
    relevant_packages: set[str] | frozenset[str],
    *,
    window: tuple[int, int] | None = None,
) -> list[dict[str, object]]:
    """Extract relevant package transactions inside a bounded time window.

    ``window`` bounds analysis to ``(start_epoch, end_epoch)``; ``None`` means
    no time filtering.  Every result carries ``proves_causation: False`` — a
    package change in the window is evidence, not proof of a regression.
    """

    changes: list[dict[str, object]] = []
    for line in log_text.splitlines():
        transaction = _parse_transaction(line.strip())
        if transaction is None:
            continue
        package = transaction["package"]
        if package not in relevant_packages:
            continue
        stamp = transaction["stamp"]
        assert isinstance(stamp, str)
        timestamp = _stamp_epoch(stamp)
        if window is not None:
            if timestamp is None or not window[0] <= timestamp <= window[1]:
                continue
        changes.append({
            "format_version": STATE_FORMAT_VERSION,
            "type": "PACKAGE_TRANSACTION",
            "action": transaction["action"],
            "package": package,
            "previous": transaction["previous"],
            "current": transaction["current"],
            "timestamp": stamp,
            "proves_causation": False,
        })
    return changes
