"""Phase-15 incident synthesis and root-cause grouping."""
from __future__ import annotations

import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

from ..constants import INSTALLER_VERSION
from ..context import XdgPaths
from ..models import to_jsonable
from ..verification.models import ComponentHealthState, VerificationCheckState, VerificationReport
from .codes import stage_code, verification_error_code
from .models import (
    BlockedImpact,
    BuildDiagnosticSummary,
    DiagnosticDisplay,
    DiagnosticEnvironment,
    DiagnosticEvent,
    DiagnosticReport,
    DiagnosticSeverity,
    RollbackAvailability,
    RootFailure,
    VerificationDiagnosticSummary,
)
from .normalize import PathNormalizer


class DiagnosticReportBuilder:
    """Build one privacy-reduced report from authoritative installer observations."""

    def __init__(
        self,
        *,
        paths: XdgPaths,
        source_root: Path,
        transaction_id: str,
        snapshot,
        plan=None,
        verification: VerificationReport | None = None,
        build_report=None,
        transaction_dir: Path | None = None,
        now: datetime | None = None,
    ) -> None:
        self.paths = paths
        self.source_root = Path(source_root)
        self.transaction_id = transaction_id
        self.snapshot = snapshot
        self.plan = plan
        self.verification = verification
        self.build_report = build_report
        self.transaction_dir = transaction_dir
        self.now = now or datetime.now(timezone.utc)
        self.normalizer = PathNormalizer(paths=paths, source_root=self.source_root)

    def build(self) -> DiagnosticReport:
        environment = self._environment()
        blocked = self._blocked_components()
        roots = self._root_failures(blocked)
        fingerprint = self._fingerprint(roots, environment)
        transaction_tag = hashlib.sha256(self.transaction_id.encode("utf-8")).hexdigest()[:4].upper()
        incident_id = f"RH-DIAG-{self.now.strftime('%Y%m%d-%H%M%S')}-{fingerprint[:8].upper()}-{transaction_tag}"
        events = self._events(roots)
        verification = self._verification_summary()
        warnings = self._warnings()
        return DiagnosticReport(
            schema_version=1,
            incident_id=incident_id,
            incident_fingerprint=fingerprint,
            created_at=self.now.astimezone(timezone.utc).isoformat(),
            installer_version=INSTALLER_VERSION,
            transaction_id=self.transaction_id,
            mode=getattr(getattr(self.plan, "mode", None), "value", None),
            installation_origin=getattr(getattr(self.snapshot, "installation", None), "origin", None).value if getattr(getattr(self.snapshot, "installation", None), "origin", None) is not None else None,
            current_version=getattr(self.plan, "current_version", None) if self.plan is not None else getattr(getattr(self.snapshot, "installation", None), "installed_version_text", None),
            target_version=getattr(self.plan, "target_version", None) if self.plan is not None else getattr(getattr(getattr(self.snapshot, "installation", None), "source", None), "version_text", None),
            manifest_digest=getattr(self.plan, "manifest_digest", None) if self.plan is not None else getattr(getattr(self.snapshot, "manifest", None), "digest", None),
            plan_digest=getattr(self.plan, "plan_digest", None),
            environment=environment,
            verification=verification,
            build=self._build_summary(),
            events=events,
            root_failures=roots,
            blocked_components=blocked,
            warnings=warnings,
            rollback=self._rollback(),
            privacy_contract=(
                "environment fields are allowlisted; arbitrary process environment is never serialized",
                "home/XDG/source/temp paths are normalized",
                "private file contents and command stdout/stderr are not included",
                "mutable user configuration is described by check state, not embedded content",
            ),
        )

    def _environment(self) -> DiagnosticEnvironment:
        snap = self.snapshot
        distro = snap.distro
        session = snap.session
        hypr = snap.hyprland
        displays = tuple(
            DiagnosticDisplay(
                name=str(item.name),
                width=item.width,
                height=item.height,
                refresh_hz=item.refresh_hz,
                scale=item.scale,
                x=item.x,
                y=item.y,
                focused=item.focused,
            )
            for item in snap.displays
            if not getattr(item, "disabled", False)
        )
        version = str(hypr.version) if getattr(hypr, "version", None) is not None else (hypr.raw_version or None)
        return DiagnosticEnvironment(
            distribution_id=distro.id,
            distribution=distro.pretty_name,
            distribution_version=distro.version_id,
            architecture=snap.architecture,
            kernel=snap.kernel,
            package_manager=snap.package_manager.kind,
            session_type=session.session_type,
            wayland=bool(session.wayland),
            systemd_user_available=bool(session.systemd_user_available),
            hyprland_version=version,
            hyprland_compatibility=hypr.compatibility.value,
            hyprland_commit=hypr.commit,
            hyprland_abi_hash=hypr.abi_hash,
            hyprland_dirty=hypr.dirty,
            displays=displays,
        )

    def _verification_summary(self) -> VerificationDiagnosticSummary:
        report = self.verification
        if report is None:
            return VerificationDiagnosticSummary(None, None, None, 0, 0, 0)
        return VerificationDiagnosticSummary(
            install_health=report.install_health.value,
            activation_state=report.activation.state.value,
            runtime_health=report.activation.runtime_health.value,
            component_count=len(report.components),
            check_count=len(report.checks),
            immutable_artifact_count=sum(1 for item in report.artifacts if item.sha256 or item.immutable_fingerprint),
        )

    def _build_summary(self) -> BuildDiagnosticSummary | None:
        report = self.build_report
        if report is None:
            return None
        provenance = report.provenance
        return BuildDiagnosticSummary(
            state=report.state.value,
            configured=report.configured,
            required_targets_built=report.required_targets_built,
            self_checks_passed=report.self_checks_passed,
            staged_install_completed=report.staged_install_completed,
            live_targets_unchanged=report.live_targets_unchanged,
            staged_payload_bytes=report.staged_payload_bytes,
            realmheart_version=provenance.realmheart_version if provenance else None,
            source_revision=provenance.source_revision if provenance else None,
            source_dirty=provenance.source_dirty if provenance else None,
            hyprland_version=provenance.hyprland_version if provenance else None,
            hyprland_commit=provenance.hyprland_commit if provenance else None,
            hyprland_abi_hash=provenance.hyprland_abi_hash if provenance else None,
            fx_build_id=provenance.fx_build_id if provenance else None,
        )

    def _blocked_components(self) -> tuple[BlockedImpact, ...]:
        if self.verification is None:
            return ()
        return tuple(
            BlockedImpact(item.component_id, item.display_name, item.blocked_by)
            for item in self.verification.components
            if item.state is ComponentHealthState.BLOCKED
        )

    def _root_failures(self, blocked: tuple[BlockedImpact, ...]) -> tuple[RootFailure, ...]:
        if self.verification is None:
            blockers = tuple(getattr(self.plan, "blockers", ()) if self.plan is not None else getattr(self.snapshot, "blockers", ()))
            roots: list[RootFailure] = []
            if blockers:
                for index, blocker in enumerate(blockers, start=1):
                    code = "RH_PLAN_BLOCKED" if self.plan is not None else "RH_PREFLIGHT_BLOCKED"
                    roots.append(RootFailure(
                        root_id=f"root-{index:02d}",
                        component_id=None,
                        component_name=None,
                        severity=DiagnosticSeverity.CRITICAL,
                        error_codes=(code,),
                        failed_checks=(),
                        summary=self.normalizer.text(blocker) or "installer planning is blocked",
                        affected_components=(),
                    ))
                return tuple(roots)
            if self.build_report is not None and not self.build_report.ok:
                failed_units = [item for item in self.build_report.build_units if not item.ok]
                if failed_units:
                    unit_by_id = {item.id: item for item in getattr(self.plan, "build_units", ())} if self.plan is not None else {}
                    for index, item in enumerate(failed_units, start=1):
                        unit = unit_by_id.get(item.build_unit_id)
                        affected = tuple(unit.component_ids) if unit is not None else ()
                        token = ''.join(ch if ch.isalnum() else '_' for ch in item.build_unit_id.upper()).strip('_')
                        roots.append(RootFailure(
                            root_id=f"root-{index:02d}",
                            component_id=affected[0] if len(affected) == 1 else None,
                            component_name=item.build_unit_id,
                            severity=DiagnosticSeverity.CRITICAL,
                            error_codes=(f"RH_BUILD_{token}_FAILED",),
                            failed_checks=(),
                            summary=f"native BuildUnit {item.build_unit_id} failed",
                            affected_components=tuple(sorted(set(affected))),
                        ))
                else:
                    roots.append(RootFailure(
                        root_id="root-01",
                        component_id=None,
                        component_name="Native build/stage",
                        severity=DiagnosticSeverity.CRITICAL,
                        error_codes=("RH_BUILD_STAGE_FAILED",),
                        failed_checks=(),
                        summary="native build or staged-install validation failed",
                        affected_components=(),
                    ))
                return tuple(roots)
            return ()

        failed_components = [item for item in self.verification.components if item.state is ComponentHealthState.FAILED]
        fx_failed = any(item.component_id == "realmheart-fx" for item in failed_components)
        roots: list[RootFailure] = []
        root_index = 0
        for component in failed_components:
            failed_checks = tuple(check for check in component.checks if check.state is VerificationCheckState.FAILED)
            # Required-FX promotion is a causal bridge, not a second root cause.
            if component.component_id == "realmheart-core" and fx_failed and failed_checks and all(check.id == "verify.core.required-fx" for check in failed_checks):
                continue
            root_index += 1
            codes = tuple(sorted({verification_error_code(check.id) for check in failed_checks})) or ("RH_COMPONENT_FAILED",)
            check_ids = tuple(check.id for check in failed_checks)
            summary = f"{component.display_name} failed verification"
            if failed_checks:
                summary += f" at {failed_checks[0].id}"
            affected = self._affected_components(component.component_id, blocked)
            if component.component_id == "realmheart-fx" and fx_failed:
                core = next((item for item in failed_components if item.component_id == "realmheart-core"), None)
                if core and core.component_id not in affected:
                    affected = tuple(sorted(set(affected) | {core.component_id}))
            category = component.category
            severity = DiagnosticSeverity.CRITICAL if category in {"core", "fx"} else DiagnosticSeverity.ERROR if category == "essential" else DiagnosticSeverity.WARNING
            roots.append(RootFailure(
                root_id=f"root-{root_index:02d}",
                component_id=component.component_id,
                component_name=component.display_name,
                severity=severity,
                error_codes=codes,
                failed_checks=check_ids,
                summary=summary,
                affected_components=affected,
            ))
        return tuple(roots)

    def _affected_components(self, root: str, blocked: tuple[BlockedImpact, ...]) -> tuple[str, ...]:
        affected = {root}
        changed = True
        while changed:
            changed = False
            for item in blocked:
                if item.component_id in affected:
                    continue
                if any(dep in affected for dep in item.blocked_by):
                    affected.add(item.component_id)
                    changed = True
        return tuple(sorted(affected))

    def _events(self, roots: tuple[RootFailure, ...]) -> tuple[DiagnosticEvent, ...]:
        events: list[DiagnosticEvent] = []
        def add(stage: str, severity: DiagnosticSeverity, code: str, summary: str, component_id: str | None = None, check_id: str | None = None) -> None:
            events.append(DiagnosticEvent(len(events) + 1, stage, severity, code, self.normalizer.text(summary) or summary, component_id, check_id))
        add("preflight", DiagnosticSeverity.INFO if self.snapshot.ready else DiagnosticSeverity.ERROR, stage_code("preflight", self.snapshot.state.value), f"preflight state: {self.snapshot.state.value}")
        if self.plan is not None:
            add("planning", DiagnosticSeverity.INFO if self.plan.ready else DiagnosticSeverity.ERROR, stage_code("plan", self.plan.state.value), f"installation plan: {self.plan.state.value}")
        if self.build_report is not None:
            add("build_stage", DiagnosticSeverity.INFO if self.build_report.ok else DiagnosticSeverity.ERROR, stage_code("build_stage", self.build_report.state.value), f"native build/stage: {self.build_report.state.value}")
        if self.verification is not None:
            severity = DiagnosticSeverity.INFO if self.verification.ok else DiagnosticSeverity.ERROR
            add("verification", severity, stage_code("verification", self.verification.install_health.value), f"install health: {self.verification.install_health.value}")
        for root in roots:
            add("diagnostics", root.severity, root.error_codes[0], root.summary, root.component_id, root.failed_checks[0] if root.failed_checks else None)
        return tuple(events)

    def _warnings(self) -> tuple[str, ...]:
        raw: list[str] = []
        raw.extend(getattr(self.snapshot, "warnings", ()))
        if self.plan is not None:
            raw.extend(getattr(self.plan, "warnings", ()))
        if self.build_report is not None:
            raw.extend(getattr(self.build_report, "warnings", ()))
        if self.verification is not None:
            raw.extend(getattr(self.verification, "warnings", ()))
        result: list[str] = []
        seen: set[str] = set()
        for item in raw:
            safe = self.normalizer.text(item)
            if safe and safe not in seen:
                seen.add(safe)
                result.append(safe)
        return tuple(result)

    def _rollback(self) -> RollbackAvailability:
        baseline = self.paths.baseline_backup.exists() and not self.paths.baseline_backup.is_symlink()
        version_count = 0
        if self.paths.version_backups.is_dir() and not self.paths.version_backups.is_symlink():
            try:
                version_count = sum(1 for item in self.paths.version_backups.iterdir() if item.is_dir() and not item.is_symlink())
            except OSError:
                version_count = 0
        recovery_candidates = 0
        if self.paths.transactions.is_dir() and not self.paths.transactions.is_symlink():
            try:
                for txdir in self.paths.transactions.iterdir():
                    summary = txdir / "transaction.json"
                    if not txdir.is_dir() or txdir.is_symlink() or not summary.is_file():
                        continue
                    try:
                        payload = json.loads(summary.read_text(encoding="utf-8"))
                    except (OSError, json.JSONDecodeError):
                        recovery_candidates += 1
                        continue
                    if payload.get("state") in {"applying", "verifying", "interrupted", "rolling_back", "rollback_failed", "failed"}:
                        recovery_candidates += 1
            except OSError:
                recovery_candidates = 0
        journal = bool(self.transaction_dir and (self.transaction_dir / "journal.jsonl").is_file())
        possible = True if journal else (None if recovery_candidates else False)
        return RollbackAvailability(
            permanent_baseline_available=baseline,
            version_snapshot_count=version_count,
            recovery_candidate_count=recovery_candidates,
            transaction_journal_available=journal,
            automatic_recovery_possible=possible,
            note="Phase 15 reports availability only; final keep/rollback decisions are Phase 16.",
        )

    def _fingerprint(self, roots: tuple[RootFailure, ...], environment: DiagnosticEnvironment) -> str:
        payload = {
            "roots": [
                {"component": item.component_id, "codes": item.error_codes}
                for item in roots
            ],
            "target_version": getattr(self.plan, "target_version", None),
            "hyprland_version": environment.hyprland_version,
            "manifest": getattr(self.plan, "manifest_digest", None),
        }
        encoded = json.dumps(to_jsonable(payload), sort_keys=True, separators=(",", ":")).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()
