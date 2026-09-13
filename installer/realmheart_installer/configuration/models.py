"""Structured Phase-11 configuration/terminal integration results."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class MutationResult:
    action_id: str
    target: str
    changed: bool
    before_fingerprint: str
    after_fingerprint: str
    operation_ids: tuple[str, ...] = ()
    detail: str | None = None


@dataclass(frozen=True)
class GeneratedArtifactResult:
    artifact_id: str
    path: str
    exists: bool
    size: int | None
    fingerprint: str


@dataclass(frozen=True)
class VerificationResult:
    id: str
    ok: bool
    detail: str


@dataclass(frozen=True)
class UnitState:
    enabled: bool
    active: bool
    enabled_text: str
    active_text: str


@dataclass(frozen=True)
class ServiceIntegrationResult:
    attempted: bool
    before: UnitState | None
    after: UnitState | None
    commands: tuple[tuple[str, ...], ...]
    ok: bool
    detail: str
    operation_id: str | None = None


@dataclass(frozen=True)
class ConfigurationIntegrationReport:
    transaction_id: str
    mutations: tuple[MutationResult, ...]
    generated_artifacts: tuple[GeneratedArtifactResult, ...]
    verification: tuple[VerificationResult, ...]
    service: ServiceIntegrationResult
    warnings: tuple[str, ...]
    blockers: tuple[str, ...]
    rolled_back: bool

    @property
    def ok(self) -> bool:
        return not self.blockers and all(item.ok for item in self.verification) and self.service.ok
