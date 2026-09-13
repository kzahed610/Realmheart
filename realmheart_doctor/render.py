"""Terminal rendering for the read-only Doctor acceptance verdict."""
from __future__ import annotations

from .models import AcceptanceAssessment


def render_acceptance_assessment(assessment: AcceptanceAssessment) -> str:
    lines = [
        "Realmheart Doctor — installation acceptance",
        "",
        f"Recommendation ..... {assessment.recommendation.value.upper()}",
        f"Realmheart ......... {assessment.realmheart_version}",
        f"Transaction ........ {assessment.transaction_id}",
        f"Artifacts checked .. {assessment.checked_artifacts}",
        f"Capabilities checked {assessment.checked_capabilities}",
        f"Activation ......... {assessment.activation_state}",
        f"Runtime health ..... {assessment.runtime_health}",
        "",
        assessment.summary,
    ]
    if assessment.findings:
        lines += ["", "Findings"]
        for finding in assessment.findings:
            lines.append(f"  [{finding.severity.upper()}] {finding.subject}: {finding.summary}")
    return "\n".join(lines)
