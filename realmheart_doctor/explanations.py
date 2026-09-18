"""Evidence-backed explanations for observed failure classes.

An explanation explains the class that was actually classified and names the
evidence behind it.  It never fabricates a cause, and a failure class without a
template stays explicit rather than receiving invented prose.
"""
from __future__ import annotations

_TEMPLATES = {
    "COMPONENT_ARTIFACT_MISSING": (
        "Declared component files are missing from their canonical paths. "
        "The component cannot start until they are restored."
    ),
    "COMPONENT_ARTIFACT_INVALID": (
        "A declared component file exists with the wrong filesystem shape "
        "(type, symlink, or mode), so the component cannot use it."
    ),
    "COMPONENT_ARTIFACT_CORRUPT": (
        "A declared component file does not match the accepted installation "
        "digest or fingerprint; the installed tree changed after acceptance."
    ),
    "DEPENDENCY_MISSING": (
        "A required runtime dependency or capability was observed missing. "
        "Check the affected packages before suspecting Realmheart itself."
    ),
    "DEPENDENCY_VERSION_MISMATCH": (
        "An observed dependency version violates the declared compatibility "
        "contract. This usually follows a system update; a rebuild can realign "
        "the component, but a dependency downgrade is never automatic."
    ),
    "OBSERVED_FAILURE": (
        "A declared health check failed without matching a more specific "
        "failure class. The evidence below is the primary symptom."
    ),
    "REPAIR_ATTEMPT": (
        "This incident records a manual repair attempt rather than a health "
        "regression; the attempt and its verification are part of the record."
    ),
}


def explanation_for(failure_class: str, evidence: tuple[str, ...] = ()) -> str | None:
    """Return an explanation for the class, or ``None`` when one does not exist."""

    template = _TEMPLATES.get(failure_class)
    if template is None:
        return None
    if not evidence:
        return template
    return template + " Observed evidence: " + ", ".join(evidence[:4]) + "."


def render_explanations(explanations: tuple[dict[str, object], ...]) -> str:
    lines: list[str] = []
    for item in explanations:
        lines.append(f"{item['component']}: {item['failure_class']}")
        lines.append(f"  {item['explanation']}")
    return "\n".join(lines)
