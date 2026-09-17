"""Conservative comparisons of recorded evidence, never causal attribution."""
from __future__ import annotations


def changes_since_healthy(current: dict, last_healthy: dict | None) -> list[dict]:
    """Compare only evidence present in both snapshots.

    Missing history is not a change. Version correlation alone never proves
    that an update caused a component failure.
    """
    if last_healthy is None:
        return []
    changes = []
    for field, kind in (("release_version", "REALMHEART_VERSION_CHANGED"),
                        ("manifest_digest", "MANIFEST_CHANGED")):
        before, after = last_healthy.get(field), current.get(field)
        if before is not None and after is not None and before != after:
            changes.append({"type": kind, "subject": "realmheart", "previous": before,
                            "current": after, "timestamp": current.get("captured_at"),
                            "proves_causation": False})
    return changes
