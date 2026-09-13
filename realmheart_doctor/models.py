"""Read-only Realmheart Doctor acceptance models."""
from __future__ import annotations

from dataclasses import asdict, dataclass
from enum import Enum


class AcceptanceRecommendation(str, Enum):
    KEEP = "keep"
    KEEP_WITH_WARNINGS = "keep_with_warnings"
    REVERT_RECOMMENDED = "revert_recommended"
    INDETERMINATE = "indeterminate"


@dataclass(frozen=True)
class AcceptanceFinding:
    code: str
    severity: str
    subject: str
    summary: str


@dataclass(frozen=True)
class AcceptanceAssessment:
    schema_version: int
    recommendation: AcceptanceRecommendation
    transaction_id: str
    realmheart_version: str
    manifest_digest: str
    checked_artifacts: int
    checked_capabilities: int
    activation_state: str
    runtime_health: str
    findings: tuple[AcceptanceFinding, ...]
    forensic_drift_count: int
    forensic_incident_count: int
    summary: str

    def to_dict(self) -> dict[str, object]:
        payload = asdict(self)
        payload["recommendation"] = self.recommendation.value
        return payload
