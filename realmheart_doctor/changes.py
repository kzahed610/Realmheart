"""Conservative comparisons of recorded evidence, never causal attribution."""
from __future__ import annotations


def _indexed(entries, key: str) -> dict[str, dict]:
    if not isinstance(entries, list):
        return {}
    result: dict[str, dict] = {}
    for item in entries:
        if not isinstance(item, dict):
            continue
        identity = item.get(key)
        if isinstance(identity, str) and identity:
            result[identity] = item
    return result


def changes_since_healthy(
    current: dict,
    last_healthy: dict | None,
    *,
    current_component: dict | None = None,
) -> list[dict]:
    """Compare only evidence present in both snapshots.

    Missing history is not a change. Version correlation alone never proves
    that an update caused a component failure.  Newer state records also carry
    component runtime-capability and build-fingerprint evidence; older state
    files remain valid and simply produce fewer comparisons.
    """
    if last_healthy is None:
        return []
    changes: list[dict] = []
    timestamp = current.get("captured_at")
    for field, kind in (("release_version", "REALMHEART_VERSION_CHANGED"),
                        ("manifest_digest", "MANIFEST_CHANGED")):
        before, after = last_healthy.get(field), current.get(field)
        if before is not None and after is not None and before != after:
            changes.append({"type": kind, "subject": "realmheart", "previous": before,
                            "current": after, "timestamp": timestamp,
                            "proves_causation": False})

    if current_component is None:
        component_id = last_healthy.get("component_id")
        components = current.get("components")
        if isinstance(component_id, str) and isinstance(components, dict):
            candidate = components.get(component_id)
            if isinstance(candidate, dict):
                current_component = candidate
    if not isinstance(current_component, dict):
        return changes

    before_caps = _indexed(last_healthy.get("capabilities"), "capability_id")
    after_caps = _indexed(current_component.get("capabilities"), "capability_id")
    for capability_id in sorted(before_caps.keys() & after_caps.keys()):
        before = before_caps[capability_id]
        after = after_caps[capability_id]
        before_state, after_state = before.get("state"), after.get("state")
        if before_state is not None and after_state is not None and before_state != after_state:
            changes.append({
                "type": "CAPABILITY_STATE_CHANGED",
                "subject": capability_id,
                "previous": before_state,
                "current": after_state,
                "timestamp": timestamp,
                "proves_causation": False,
            })
        before_version, after_version = before.get("version"), after.get("version")
        if (before_version is not None and after_version is not None
                and before_version != after_version):
            changes.append({
                "type": "DEPENDENCY_VERSION_CHANGED",
                "subject": str(after.get("dependency_id") or before.get("dependency_id") or capability_id),
                "capability_id": capability_id,
                "previous": before_version,
                "current": after_version,
                "timestamp": timestamp,
                "proves_causation": False,
            })

    before_build = _indexed(last_healthy.get("build_fingerprints"), "dependency_id")
    after_build = _indexed(current_component.get("build_fingerprints"), "dependency_id")
    for dependency_id in sorted(before_build.keys() & after_build.keys()):
        before = before_build[dependency_id]
        after = after_build[dependency_id]
        before_build_version, after_build_version = before.get("build_version"), after.get("build_version")
        if (before_build_version is not None and after_build_version is not None
                and before_build_version != after_build_version):
            changes.append({
                "type": "BUILD_DEPENDENCY_VERSION_CHANGED",
                "subject": dependency_id,
                "previous": before_build_version,
                "current": after_build_version,
                "timestamp": timestamp,
                "proves_causation": False,
            })
        before_current, after_current = before.get("current_version"), after.get("current_version")
        if (before_current is not None and after_current is not None
                and before_current != after_current):
            changes.append({
                "type": "DEPENDENCY_VERSION_CHANGED",
                "subject": dependency_id,
                "previous": before_current,
                "current": after_current,
                "timestamp": timestamp,
                "proves_causation": False,
            })
    return changes
