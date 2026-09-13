"""Phase-13 observed-state verification engine.

Verification is read-only.  Desired state comes from the approved InstallationPlan
and canonical manifest, but every health/receipt assertion is derived from what
is actually observed on disk or through bounded status probes.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import tempfile
from collections import defaultdict
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

from realmheart_maintenance.manifest import ManifestRegistry

from ..configuration.terminal import KITTY_BEGIN, KITTY_END, kitty_managed_body, render_action_content, validate_terminal_state
from ..constants import INSTALLED_STATE_SCHEMA_VERSION, INSTALLER_VERSION
from ..context import XdgPaths
from ..environment.capabilities import CapabilityResult, CapabilityScanner, CapabilityState, DependencyLifecycle, RequirementLevel
from ..filesystem.compare import fingerprint_path
from ..models import FxCompatibility
from ..native_build.models import BuildStageReport
from ..planning.models import ConfigActionKind, InstallationPlan, ServiceActionKind
from .models import (
    ActivationState,
    ActivationVerification,
    ComponentHealthState,
    ComponentVerification,
    FxReceiptInput,
    FxRebuildTriggerInput,
    FxRuntimeIdentity,
    InstallHealthState,
    ObservedArtifactIdentity,
    ObservedDependency,
    ReceiptBuildUnitInput,
    ReceiptInputAssembly,
    RuntimeHealthState,
    VerificationCheckResult,
    VerificationCheckState,
    VerificationClass,
    VerificationReport,
)

REPORT_SCHEMA_VERSION = 1


class VerificationEngine:
    def __init__(
        self,
        *,
        plan: InstallationPlan,
        registry: ManifestRegistry,
        runner,
        paths: XdgPaths,
        source_root: Path,
        build_report: BuildStageReport | None = None,
        capability_results: tuple[CapabilityResult, ...] | None = None,
        verified_at: datetime | None = None,
        privileged_uid: int = 0,
        privileged_gid: int = 0,
    ) -> None:
        self.plan = plan
        self.registry = registry
        self.runner = runner
        self.paths = paths
        self.source_root = Path(source_root)
        self.build_report = build_report
        self._capability_results = capability_results
        self.verified_at = verified_at or datetime.now(timezone.utc)
        self.privileged_uid = privileged_uid
        self.privileged_gid = privileged_gid

    def run(self) -> VerificationReport:
        if self.plan.manifest_digest != self.registry.digest:
            raise ValueError("InstallationPlan manifest digest no longer matches canonical manifest")
        if not self.plan.ready:
            raise ValueError("refusing post-install verification for a blocked InstallationPlan")

        artifacts = self._observe_artifacts()
        artifact_by_id = {item.artifact_id: item for item in artifacts}
        capabilities = self._observe_capabilities()
        dependencies = tuple(self._dependency_record(item, observed=True) for item in capabilities)

        checks: list[VerificationCheckResult] = []
        checks.extend(self._manifest_structural_checks(artifact_by_id))
        checks.extend(self._artifact_contract_checks(artifact_by_id))
        checks.extend(self._stage_identity_checks(artifact_by_id))
        checks.extend(self._dependency_checks(capabilities))
        checks.extend(self._configuration_checks())
        checks.extend(self._service_checks())
        checks.extend(self._security_checks(artifact_by_id))
        checks.extend(self._component_smoke_checks())
        fx_runtime = self._probe_fx_runtime()
        checks.extend(self._fx_checks(artifact_by_id, fx_runtime))
        checks.extend(self._build_provenance_checks())

        components = self._aggregate_components(tuple(checks))
        components, fx_core_gate = self._promote_required_fx_failure_to_core(components)
        if fx_core_gate is not None:
            checks.append(fx_core_gate)
        activation = self._activation(components, fx_runtime)
        install_health, blockers, warnings = self._overall_health(components)
        receipt = self._receipt_inputs(
            install_health=install_health,
            activation=activation,
            components=components,
            artifacts=artifacts,
            dependencies=dependencies,
        )
        return VerificationReport(
            schema_version=REPORT_SCHEMA_VERSION,
            transaction_id=self.plan.transaction_id,
            realmheart_version=self.plan.target_version,
            manifest_digest=self.plan.manifest_digest,
            plan_digest=self.plan.plan_digest,
            install_health=install_health,
            activation=activation,
            components=components,
            checks=tuple(checks),
            artifacts=artifacts,
            dependencies=dependencies,
            receipt_inputs=receipt,
            warnings=warnings,
            blockers=blockers,
        )

    # ---- observed artifacts -------------------------------------------------

    def _observe_artifacts(self) -> tuple[ObservedArtifactIdentity, ...]:
        result: list[ObservedArtifactIdentity] = []
        for action in self.plan.artifact_actions:
            path = Path(action.target)
            spec = self.registry.artifacts[action.artifact_id]
            exists = path.exists() or path.is_symlink()
            fs_type = None
            mode = None
            uid = gid = size = None
            sha = fingerprint = None
            if exists:
                try:
                    st = path.lstat()
                    mode = f"{stat.S_IMODE(st.st_mode):04o}"
                    uid, gid = st.st_uid, st.st_gid
                    if stat.S_ISREG(st.st_mode):
                        fs_type = "file"
                        size = st.st_size
                    elif stat.S_ISDIR(st.st_mode):
                        fs_type = "directory"
                    elif stat.S_ISLNK(st.st_mode):
                        fs_type = "symlink"
                    else:
                        fs_type = "other"
                    # Immutable receipt identity is intentionally limited to
                    # release/system ownership. Never hash mutable/shared user
                    # data simply to fill a receipt.
                    if spec.ownership in {"release", "system"} and fs_type in {"file", "directory"}:
                        if fs_type == "file":
                            sha = _sha256_file(path)
                        fingerprint = fingerprint_path(path)
                except OSError:
                    pass
            result.append(ObservedArtifactIdentity(
                artifact_id=action.artifact_id,
                component_id=action.component_id,
                path=str(path),
                artifact_type=action.artifact_type,
                ownership=action.ownership,
                required=action.required,
                exists=exists,
                filesystem_type=fs_type,
                mode=mode,
                uid=uid,
                gid=gid,
                size_bytes=size,
                sha256=sha,
                immutable_fingerprint=fingerprint,
            ))
        return tuple(result)

    def _manifest_structural_checks(self, artifacts: dict[str, ObservedArtifactIdentity]) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        for planned in self.plan.health_checks:
            if "install_verify" not in planned.contexts:
                continue
            observed = artifacts.get(planned.artifact_id or "")
            if observed is None:
                checks.append(self._check(planned.id, planned.component_id, VerificationClass.STRUCTURAL, VerificationCheckState.FAILED, "health check references an unobserved artifact", artifact_id=planned.artifact_id, critical=self._critical(planned.component_id)))
                continue
            if planned.check == "artifact_exists":
                state = VerificationCheckState.PASS if observed.exists else VerificationCheckState.FAILED
                summary = "artifact exists" if observed.exists else "required artifact is missing"
            elif planned.check == "artifact_executable":
                executable = observed.exists and observed.filesystem_type == "file" and observed.mode is not None and bool(int(observed.mode, 8) & 0o111)
                state = VerificationCheckState.PASS if executable else VerificationCheckState.FAILED
                summary = "artifact is executable" if executable else "artifact is not an executable regular file"
            else:
                state = VerificationCheckState.FAILED
                summary = f"unsupported install verification check: {planned.check}"
            checks.append(self._check(planned.id, planned.component_id, VerificationClass.STRUCTURAL, state, summary, artifact_id=planned.artifact_id, observed=observed.path, critical=self._critical(planned.component_id)))
        return tuple(checks)

    def _artifact_contract_checks(self, artifacts: dict[str, ObservedArtifactIdentity]) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        for artifact_id, observed in artifacts.items():
            spec = self.registry.artifacts[artifact_id]
            if not spec.required:
                continue
            expected_type = "directory" if spec.type == "directory" else "file"
            type_ok = observed.exists and observed.filesystem_type == expected_type
            checks.append(self._check(
                f"verify.artifact.{artifact_id}.type",
                spec.component_id,
                VerificationClass.STRUCTURAL,
                VerificationCheckState.PASS if type_ok else VerificationCheckState.FAILED,
                "filesystem type matches canonical artifact" if type_ok else "artifact has wrong filesystem type or is a symlink",
                artifact_id=artifact_id,
                observed=observed.filesystem_type,
                expected=expected_type,
                critical=self._critical(spec.component_id),
            ))
            if spec.mode and observed.exists:
                mode_ok = observed.mode == spec.mode
                checks.append(self._check(
                    f"verify.artifact.{artifact_id}.mode",
                    spec.component_id,
                    VerificationClass.STRUCTURAL,
                    VerificationCheckState.PASS if mode_ok else VerificationCheckState.FAILED,
                    "artifact mode matches canonical contract" if mode_ok else "artifact mode differs from canonical contract",
                    artifact_id=artifact_id,
                    observed=observed.mode,
                    expected=spec.mode,
                    critical=self._critical(spec.component_id),
                ))
            if spec.type == "generated" and observed.exists:
                nonempty = (observed.size_bytes or 0) > 0
                checks.append(self._check(
                    f"verify.artifact.{artifact_id}.nonempty",
                    spec.component_id,
                    VerificationClass.STRUCTURAL,
                    VerificationCheckState.PASS if nonempty else VerificationCheckState.FAILED,
                    "generated artifact is non-empty" if nonempty else "generated artifact is empty",
                    artifact_id=artifact_id,
                    observed=str(observed.size_bytes or 0),
                    expected=">0 bytes",
                ))
        return tuple(checks)

    def _stage_identity_checks(self, artifacts: dict[str, ObservedArtifactIdentity]) -> tuple[VerificationCheckResult, ...]:
        if self.build_report is None:
            return ()
        checks: list[VerificationCheckResult] = []
        for staged in self.build_report.artifacts:
            observed = artifacts.get(staged.artifact_id)
            if observed is None or not staged.ok:
                continue
            if staged.sha256 is not None:
                matches = observed.sha256 == staged.sha256
                observed_identity = observed.sha256
                expected_identity = staged.sha256
            elif staged.fingerprint is not None:
                matches = observed.immutable_fingerprint == staged.fingerprint
                observed_identity = observed.immutable_fingerprint
                expected_identity = staged.fingerprint
            else:
                continue
            component_id = self.registry.artifacts[staged.artifact_id].component_id
            checks.append(self._check(
                f"verify.provenance.{staged.artifact_id}.stage-identity",
                component_id,
                VerificationClass.PROVENANCE,
                VerificationCheckState.PASS if matches else VerificationCheckState.FAILED,
                "live immutable artifact matches validated DESTDIR payload" if matches else "live immutable artifact differs from the validated DESTDIR payload",
                artifact_id=staged.artifact_id,
                observed=observed_identity,
                expected=expected_identity,
                critical=self._critical(component_id),
            ))
        return tuple(checks)

    # ---- dependencies -------------------------------------------------------

    def _observe_capabilities(self) -> tuple[CapabilityResult, ...]:
        if self._capability_results is not None:
            return self._capability_results
        with tempfile.TemporaryDirectory(prefix="realmheart-verify-") as temp:
            scanner = CapabilityScanner(self.runner, temp_root=Path(temp), registry=self.registry)
            return scanner.scan_all()

    def _dependency_checks(self, results: tuple[CapabilityResult, ...]) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        for item in results:
            component = item.component or "realmheart-core"
            runtime_relevant = any(life in {DependencyLifecycle.RUNTIME, DependencyLifecycle.VERIFICATION} for life in item.lifecycle)
            if not runtime_relevant:
                continue
            if item.state is CapabilityState.NOT_APPLICABLE:
                state = VerificationCheckState.NOT_APPLICABLE
            elif item.satisfied:
                state = VerificationCheckState.PASS
            elif item.requirement is RequirementLevel.SOFT:
                state = VerificationCheckState.WARNING
            else:
                state = VerificationCheckState.FAILED
            checks.append(self._check(
                f"verify.capability.{item.capability_id}",
                component,
                VerificationClass.DEPENDENCY,
                state,
                item.detail,
                capability_id=item.capability_id,
                observed=item.version or item.executable,
                expected=item.requirement.value,
                critical=self._critical(component) and item.requirement is not RequirementLevel.SOFT,
            ))
        return tuple(checks)

    @staticmethod
    def _dependency_record(item: CapabilityResult, *, observed: bool) -> ObservedDependency:
        return ObservedDependency(
            capability_id=item.capability_id,
            component_id=item.component,
            requirement=item.requirement.value,
            lifecycle=tuple(value.value for value in item.lifecycle),
            state=item.state.value,
            detail=item.detail,
            version=item.version,
            executable=item.executable,
            observed_during_verification=observed,
        )

    # ---- configuration ------------------------------------------------------

    def _configuration_checks(self) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        actions = {item.id: item for item in self.plan.config_actions}

        fish = actions.get("config.fish.personal")
        if fish is not None:
            try:
                current = fingerprint_path(Path(fish.target))
                ok = current == fish.precondition_fingerprint
                detail = "personal fish config remained byte-for-byte outside Realmheart ownership" if ok else "personal fish config changed during installation"
            except OSError as exc:
                ok, detail, current = False, str(exc), None
            checks.append(self._check("verify.config.fish.personal-untouched", "terminal", VerificationClass.CONFIGURATION, VerificationCheckState.PASS if ok else VerificationCheckState.FAILED, detail, observed=current, expected=fish.precondition_fingerprint))

        kitty = actions.get("config.kitty.managed-block")
        if kitty is not None:
            try:
                text = Path(kitty.target).read_text(encoding="utf-8")
                include = kitty_managed_body(kitty)
                begin_count, end_count, include_count = text.count(KITTY_BEGIN), text.count(KITTY_END), text.count(include)
                ok = begin_count == end_count == include_count == 1
                detail = f"begin={begin_count} end={end_count} include={include_count}"
            except (OSError, UnicodeError, ValueError) as exc:
                ok, detail = False, str(exc)
            checks.append(self._check("verify.config.kitty.managed-block", "terminal", VerificationClass.CONFIGURATION, VerificationCheckState.PASS if ok else VerificationCheckState.FAILED, detail))

        hypr = actions.get("config.hypr.takeover")
        if hypr is not None:
            target = Path(hypr.target)
            staging = target.parent / f".realmheart-{target.name}-staging-{self.plan.transaction_id}"
            no_staging = not (staging.exists() or staging.is_symlink())
            checks.append(self._check("verify.config.hypr.no-staging-residue", "hypr-integration", VerificationClass.CONFIGURATION, VerificationCheckState.PASS if no_staging else VerificationCheckState.FAILED, "no staging tree remains" if no_staging else f"staging residue remains at {staging}", critical=True))
            source = Path(hypr.source) if hypr.source else None
            missing: list[str] = []
            if source and source.is_dir() and target.is_dir():
                preserve = {Path(item).parts[0] for item in hypr.preserve if Path(item).parts}
                for source_path in source.rglob("*"):
                    relative = source_path.relative_to(source)
                    if relative.parts and relative.parts[0] in preserve:
                        continue
                    if source_path.is_file() and not source_path.is_symlink() and not (target / relative).is_file():
                        missing.append(str(relative))
            elif source:
                missing.append("<target/source tree unavailable>")
            checks.append(self._check("verify.config.hypr.release-structure", "hypr-integration", VerificationClass.CONFIGURATION, VerificationCheckState.PASS if not missing else VerificationCheckState.FAILED, "Realmheart Hypr release structure present" if not missing else "missing release paths: " + ", ".join(missing[:8]), critical=True))

        # Compare deterministic Realmheart-owned source/rendered files to the
        # approved rendered content. Shared seed/generated/read-only actions are
        # intentionally handled elsewhere.
        for action in self.plan.config_actions:
            if action.kind not in {ConfigActionKind.OWNED_FILE, ConfigActionKind.RENDERED_FILE}:
                continue
            if action.component_id not in {"terminal", "clipboard-history"}:
                continue
            try:
                expected, mode = render_action_content(action, source_root=self.source_root)
                path = Path(action.target)
                actual = path.read_bytes()
                actual_mode = stat.S_IMODE(path.stat(follow_symlinks=False).st_mode)
                ok = not path.is_symlink() and actual == expected and actual_mode == mode
                detail = "rendered/owned config matches approved plan" if ok else "rendered/owned config differs from approved content or mode"
            except Exception as exc:
                ok, detail = False, str(exc)
            checks.append(self._check(f"verify.config.{action.id}", action.component_id, VerificationClass.CONFIGURATION, VerificationCheckState.PASS if ok else VerificationCheckState.FAILED, detail, critical=self._critical(action.component_id)))

        # Reuse Phase-11 safe terminal syntax/state validators. These are
        # observational except for py_compile output redirected to temp storage.
        generated = {
            item.artifact_id: Path(item.target)
            for item in self.plan.artifact_actions
            if item.component_id == "terminal" and item.commit_class.value == "generated"
        }
        if generated:
            with tempfile.TemporaryDirectory(prefix="realmheart-terminal-verify-") as temp:
                terminal_checks = validate_terminal_state(
                    home=self.paths.home,
                    config_home=self.paths.config_home,
                    state_home=self.paths.state_home,
                    generated_paths=generated,
                    runner=self.runner,
                    pycache_root=Path(temp),
                )
            for item in terminal_checks:
                checks.append(self._check(f"verify.{item.id}", "terminal", VerificationClass.CONFIGURATION, VerificationCheckState.PASS if item.ok else VerificationCheckState.FAILED, item.detail))
        return tuple(checks)

    # ---- services / security / smoke ---------------------------------------

    def _service_checks(self) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        systemctl = self._capability_executable("runtime.systemctl") or self.runner.which("systemctl") or "systemctl"
        for action in self.plan.service_actions:
            if action.action is ServiceActionKind.DAEMON_RELOAD or action.component_id is None:
                continue
            if action.action is ServiceActionKind.INSTALL_ONLY:
                continue
            enabled = self.runner.run((systemctl, "--user", "is-enabled", action.service), timeout=4.0)
            enabled_ok = enabled.ok and enabled.stdout.strip() in {"enabled", "enabled-runtime", "static", "indirect", "generated", "linked", "linked-runtime"}
            checks.append(self._check(
                f"verify.service.{action.service}.enabled",
                action.component_id,
                VerificationClass.SERVICE,
                VerificationCheckState.PASS if enabled_ok else VerificationCheckState.FAILED,
                "user unit is enabled" if enabled_ok else _command_summary(enabled),
                observed=enabled.stdout.strip() or enabled.stderr.strip(),
                expected="enabled",
                critical=self._critical(action.component_id),
            ))
            if action.action is ServiceActionKind.ENABLE_START:
                active = self.runner.run((systemctl, "--user", "is-active", action.service), timeout=4.0)
                active_ok = active.ok and active.stdout.strip() == "active"
                checks.append(self._check(
                    f"verify.service.{action.service}.active",
                    action.component_id,
                    VerificationClass.SERVICE,
                    VerificationCheckState.PASS if active_ok else VerificationCheckState.FAILED,
                    "user unit is active" if active_ok else _command_summary(active),
                    observed=active.stdout.strip() or active.stderr.strip(),
                    expected="active",
                    critical=self._critical(action.component_id),
                ))
        return tuple(checks)

    def _security_checks(self, artifacts: dict[str, ObservedArtifactIdentity]) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        helper = artifacts.get("auth.helper")
        if helper:
            owner_ok = helper.uid == self.privileged_uid and helper.gid == self.privileged_gid
            mode_ok = helper.mode == "4755"
            checks.append(self._check("verify.security.auth-helper.owner", "lockscreen-auth", VerificationClass.SECURITY, VerificationCheckState.PASS if owner_ok else VerificationCheckState.FAILED, "auth helper owner/group are trusted" if owner_ok else "auth helper is not owned by the privileged trust root", artifact_id="auth.helper", observed=f"{helper.uid}:{helper.gid}", expected=f"{self.privileged_uid}:{self.privileged_gid}", critical=True))
            checks.append(self._check("verify.security.auth-helper.mode", "lockscreen-auth", VerificationClass.SECURITY, VerificationCheckState.PASS if mode_ok else VerificationCheckState.FAILED, "auth helper mode is exactly setuid 4755" if mode_ok else "auth helper mode is not exactly 4755", artifact_id="auth.helper", observed=helper.mode, expected="4755", critical=True))
        pam = artifacts.get("auth.pam")
        if pam:
            owner_ok = pam.uid == self.privileged_uid and pam.gid == self.privileged_gid
            mode_ok = pam.mode == "0644"
            checks.append(self._check("verify.security.pam.owner", "lockscreen-auth", VerificationClass.SECURITY, VerificationCheckState.PASS if owner_ok else VerificationCheckState.FAILED, "PAM service owner/group are trusted" if owner_ok else "PAM service has unexpected ownership", artifact_id="auth.pam", observed=f"{pam.uid}:{pam.gid}", expected=f"{self.privileged_uid}:{self.privileged_gid}", critical=True))
            checks.append(self._check("verify.security.pam.mode", "lockscreen-auth", VerificationClass.SECURITY, VerificationCheckState.PASS if mode_ok else VerificationCheckState.FAILED, "PAM service mode is 0644" if mode_ok else "PAM service mode is not 0644", artifact_id="auth.pam", observed=pam.mode, expected="0644", critical=True))
            source = self.registry.artifacts["auth.pam"].source
            if source:
                try:
                    expected = (self.source_root / source).read_bytes()
                    actual = Path(pam.path).read_bytes()
                    content_ok = actual == expected
                except OSError:
                    content_ok = False
                checks.append(self._check("verify.security.pam.content", "lockscreen-auth", VerificationClass.SECURITY, VerificationCheckState.PASS if content_ok else VerificationCheckState.FAILED, "PAM service matches canonical Realmheart policy" if content_ok else "PAM service content differs from canonical policy", artifact_id="auth.pam", critical=True))
        return tuple(checks)

    def _component_smoke_checks(self) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        core = self._artifact_target("core.binary")
        if core and Path(core).is_file():
            result = self.runner.run((core, "--version"), timeout=5.0)
            output = (result.stdout or result.stderr).strip()
            ok = result.ok and self.plan.target_version in output
            checks.append(self._check("verify.smoke.core.version", "realmheart-core", VerificationClass.SMOKE, VerificationCheckState.PASS if ok else VerificationCheckState.FAILED, "Realmheart binary reports target release" if ok else _command_summary(result), observed=output, expected=self.plan.target_version, critical=True))

        event_cli = self._artifact_target("event.cli")
        if event_cli and Path(event_cli).is_file():
            help_result = self.runner.run((event_cli, "--help"), timeout=5.0)
            checks.append(self._check("verify.smoke.event-cli.help", "event-surface", VerificationClass.SMOKE, VerificationCheckState.PASS if help_result.ok else VerificationCheckState.FAILED, "Event Surface CLI starts in non-mutating help mode" if help_result.ok else _command_summary(help_result)))
            if self._service_active("realmheart-eventd.service"):
                ping = self.runner.run((event_cli, "ping"), timeout=5.0)
                checks.append(self._check("verify.smoke.event-cli.ping", "event-surface", VerificationClass.SMOKE, VerificationCheckState.PASS if ping.ok else VerificationCheckState.FAILED, "Event Surface owner-only protocol responds" if ping.ok else _command_summary(ping), observed=(ping.stdout or ping.stderr).strip()))
            else:
                checks.append(self._check("verify.smoke.event-cli.ping", "event-surface", VerificationClass.SMOKE, VerificationCheckState.BLOCKED, "Event Surface daemon is not active; protocol probe not attempted"))

        # Native verification is an optional logical component representing the
        # installer-controlled safe contract checks already executed in Phase 10.
        if self.build_report is None:
            checks.append(self._check("verify.native-tests.build-report", "native-tests", VerificationClass.COMPONENT, VerificationCheckState.NOT_APPLICABLE, "no Phase-10 build report supplied to this observational verification run"))
        else:
            checks.append(self._check("verify.native-tests.build-report", "native-tests", VerificationClass.COMPONENT, VerificationCheckState.PASS if self.build_report.self_checks_passed else VerificationCheckState.FAILED, "installer-controlled native self-checks passed" if self.build_report.self_checks_passed else "installer-controlled native self-checks failed"))
        return tuple(checks)

    def _fx_checks(self, artifacts: dict[str, ObservedArtifactIdentity], runtime: FxRuntimeIdentity) -> tuple[VerificationCheckResult, ...]:
        checks: list[VerificationCheckResult] = []
        compatible = self.plan.fx_plan.compatibility is FxCompatibility.COMPATIBLE
        checks.append(self._check("verify.fx.compatibility", "realmheart-fx", VerificationClass.FX, VerificationCheckState.PASS if compatible else VerificationCheckState.FAILED, self.plan.fx_plan.reason, observed=self.plan.fx_plan.compatibility.value, expected=FxCompatibility.COMPATIBLE.value, critical=True))
        plugin = artifacts.get("fx.plugin")
        loader = artifacts.get("fx.loader")
        identity_ok = bool(plugin and plugin.exists and plugin.immutable_fingerprint and loader and loader.exists)
        checks.append(self._check("verify.fx.live-artifacts", "realmheart-fx", VerificationClass.FX, VerificationCheckState.PASS if identity_ok else VerificationCheckState.FAILED, "required FX plugin and loader are present with plugin identity captured" if identity_ok else "required FX plugin/loader identity is incomplete", critical=True))
        if self.build_report and self.build_report.provenance:
            p = self.build_report.provenance
            abi_ok = p.hyprland_abi_hash == self.plan.fx_plan.hyprland_abi_hash and p.hyprland_commit == self.plan.fx_plan.hyprland_commit
            checks.append(self._check("verify.fx.build-provenance", "realmheart-fx", VerificationClass.PROVENANCE, VerificationCheckState.PASS if abi_ok else VerificationCheckState.FAILED, "FX build provenance matches approved Hyprland ABI/build identity" if abi_ok else "FX build provenance does not match approved Hyprland identity", observed=p.hyprland_abi_hash, expected=self.plan.fx_plan.hyprland_abi_hash, critical=True))
        else:
            checks.append(self._check("verify.fx.build-provenance", "realmheart-fx", VerificationClass.PROVENANCE, VerificationCheckState.WARNING, "Phase-10 build provenance was not supplied; live artifact presence is verified but build identity cannot be re-associated"))
        if runtime.hyprland_commit is not None or runtime.hyprland_abi_hash is not None:
            has_validated_build = self.build_report is not None and self.build_report.provenance is not None
            runtime_matches_build = bool(
                has_validated_build
                and runtime.hyprland_commit == self.build_report.provenance.hyprland_commit
                and runtime.hyprland_abi_hash == self.build_report.provenance.hyprland_abi_hash
                and runtime.hyprland_dirty is False
            )
            checks.append(self._check(
                "verify.fx.runtime-hyprland-identity",
                "realmheart-fx",
                VerificationClass.FX,
                (VerificationCheckState.PASS if runtime_matches_build else VerificationCheckState.FAILED) if has_validated_build else VerificationCheckState.NOT_APPLICABLE,
                (
                    "running Hyprland still matches the validated FX build identity" if runtime_matches_build else
                    "running Hyprland identity drifted from the validated FX build; rebuild/restart is required"
                ) if has_validated_build else "runtime Hyprland identity observed, but no validated build report was supplied for comparison",
                observed=f"{runtime.hyprland_commit or '?'} / {runtime.hyprland_abi_hash or '?'} / dirty={runtime.hyprland_dirty}",
                expected=(
                    f"{self.build_report.provenance.hyprland_commit} / {self.build_report.provenance.hyprland_abi_hash} / dirty=False"
                    if self.build_report and self.build_report.provenance else None
                ),
                critical=True,
            ))
        activation_state = VerificationCheckState.PASS if runtime.matches_validated_build is True else VerificationCheckState.PENDING
        checks.append(self._check(
            "verify.fx.runtime-build-active",
            "realmheart-fx",
            VerificationClass.ACTIVATION,
            activation_state,
            "running compositor attests the exact validated Realmheart FX build" if runtime.matches_validated_build is True else runtime.detail,
            observed=runtime.build_id,
            expected=(self.build_report.provenance.fx_build_id if self.build_report and self.build_report.provenance else None),
        ))
        return tuple(checks)

    def _promote_required_fx_failure_to_core(
        self,
        components: tuple[ComponentVerification, ...],
    ) -> tuple[tuple[ComponentVerification, ...], VerificationCheckResult | None]:
        if not self.plan.fx_plan.required:
            return components, None
        core = next(item for item in components if item.component_id == "realmheart-core")
        fx = next(item for item in components if item.component_id == "realmheart-fx")
        if core.state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED}:
            return components, None

        if fx.state not in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED}:
            gate = self._check(
                "verify.core.required-fx",
                "realmheart-core",
                VerificationClass.FX,
                VerificationCheckState.PASS,
                "required Realmheart FX install health is satisfied; activation is evaluated separately",
                critical=True,
            )
            replaced = tuple(
                ComponentVerification(
                    item.component_id, item.display_name, item.category, item.state,
                    item.checks + ((gate,) if item.component_id == "realmheart-core" else ()),
                    item.blocked_by, item.warnings, item.artifact_ids, item.build_unit_ids,
                ) if item.component_id == "realmheart-core" else item
                for item in components
            )
            return replaced, gate

        gate = self._check(
            "verify.core.required-fx",
            "realmheart-core",
            VerificationClass.FX,
            VerificationCheckState.FAILED,
            f"required Realmheart FX install health is {fx.state.value}; Core cannot be reported healthy",
            critical=True,
        )
        replaced = tuple(
            ComponentVerification(
                item.component_id,
                item.display_name,
                item.category,
                ComponentHealthState.FAILED,
                item.checks + (gate,),
                item.blocked_by,
                item.warnings,
                item.artifact_ids,
                item.build_unit_ids,
            ) if item.component_id == "realmheart-core" else item
            for item in components
        )
        return replaced, gate

    def _probe_fx_runtime(self) -> FxRuntimeIdentity:
        hyprctl = self._capability_executable("runtime.hyprctl") or self.runner.which("hyprctl")
        if not hyprctl:
            return FxRuntimeIdentity(None, None, None, None, None, None, None, None, "hyprctl is unavailable; running FX cannot be proven")

        runtime_commit = runtime_abi = None
        runtime_dirty: bool | None = None
        version = self.runner.run((hyprctl, "version", "-j"), timeout=5.0)
        if version.ok:
            try:
                payload = json.loads(version.stdout)
                commit = payload.get("commit")
                abi = payload.get("abiHash")
                dirty = payload.get("dirty")
                runtime_commit = commit if isinstance(commit, str) and commit else None
                runtime_abi = abi if isinstance(abi, str) and abi else None
                runtime_dirty = dirty if type(dirty) is bool else None
            except (json.JSONDecodeError, TypeError):
                pass

        plugins = self.runner.run((hyprctl, "plugin", "list"), timeout=5.0)
        if not plugins.ok:
            return FxRuntimeIdentity(None, None, None, None, runtime_commit, runtime_abi, runtime_dirty, None, "unable to query Hyprland plugin list")
        listed = bool(re.search(r"realmheart[-_ ]?fx", plugins.stdout or plugins.stderr, re.IGNORECASE))
        if not listed:
            return FxRuntimeIdentity(False, False, None, None, runtime_commit, runtime_abi, runtime_dirty, False, "Realmheart FX is not loaded in the current Hyprland session")

        identity = self.runner.run((hyprctl, "realmheart-fx", "identity"), timeout=5.0)
        if not identity.ok:
            return FxRuntimeIdentity(True, False, None, None, runtime_commit, runtime_abi, runtime_dirty, False, "Realmheart FX is listed but its build-identity command is unavailable")
        values = _parse_identity_output(identity.stdout)
        build_id = values.get("build_id")
        realmheart_version = values.get("realmheart_version")
        plugin_commit = values.get("hyprland_commit")
        plugin_abi = values.get("hyprland_abi")
        provenance = self.build_report.provenance if self.build_report and self.build_report.provenance else None
        if provenance is None or not provenance.fx_build_id:
            matches = None
            detail = "Realmheart FX is loaded, but no validated Phase-10 build identity was supplied for association"
        else:
            matches = (
                build_id == provenance.fx_build_id
                and realmheart_version == self.plan.target_version
                and plugin_commit == provenance.hyprland_commit
                and plugin_abi == provenance.hyprland_abi_hash
                and runtime_commit == provenance.hyprland_commit
                and runtime_abi == provenance.hyprland_abi_hash
                and runtime_dirty is False
            )
            detail = (
                "running compositor attests the exact validated Realmheart FX build"
                if matches else
                "Realmheart FX is loaded, but its runtime build/Hyprland identity differs from the validated install; a fresh session/rebuild is required"
            )
        return FxRuntimeIdentity(True, True, build_id, realmheart_version, runtime_commit, runtime_abi, runtime_dirty, matches, detail)

    def _build_provenance_checks(self) -> tuple[VerificationCheckResult, ...]:
        if self.build_report is None or self.build_report.provenance is None:
            return (self._check("verify.provenance.build-report", "realmheart-core", VerificationClass.PROVENANCE, VerificationCheckState.WARNING, "no Phase-10 build provenance supplied"),)
        p = self.build_report.provenance
        matches = (
            self.build_report.ok
            and p.realmheart_version == self.plan.target_version
            and p.manifest_digest == self.plan.manifest_digest
            and p.plan_digest == self.plan.plan_digest
            and p.eventd_autostart == "OFF"
        )
        return (self._check("verify.provenance.build-report", "realmheart-core", VerificationClass.PROVENANCE, VerificationCheckState.PASS if matches else VerificationCheckState.FAILED, "build/stage provenance matches approved plan and side-effect policy" if matches else "build/stage provenance differs from approved plan or autostart policy", critical=True),)

    # ---- aggregation / receipt ---------------------------------------------

    def _aggregate_components(self, checks: tuple[VerificationCheckResult, ...]) -> tuple[ComponentVerification, ...]:
        by_component: dict[str, list[VerificationCheckResult]] = defaultdict(list)
        for check in checks:
            by_component[check.component_id].append(check)
        result_by_id: dict[str, ComponentVerification] = {}
        ordered: list[ComponentVerification] = []
        plan_components = {item.id: item for item in self.plan.components}
        for component_id in self.registry.component_order:
            manifest_component = self.registry.components[component_id]
            planned = plan_components[component_id]
            blocked_by = tuple(
                dep.id for dep in manifest_component.realmheart_dependencies
                if dep.required and dep.id in result_by_id and result_by_id[dep.id].state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED}
            )
            component_checks = tuple(by_component.get(component_id, ()))
            warnings = tuple(check.summary for check in component_checks if check.state is VerificationCheckState.WARNING)
            if blocked_by:
                state = ComponentHealthState.BLOCKED
            elif any(check.state is VerificationCheckState.FAILED for check in component_checks):
                state = ComponentHealthState.FAILED
            elif any(check.state in {VerificationCheckState.WARNING, VerificationCheckState.BLOCKED} for check in component_checks):
                state = ComponentHealthState.DEGRADED
            elif component_checks and all(check.state is VerificationCheckState.NOT_APPLICABLE for check in component_checks):
                state = ComponentHealthState.NOT_APPLICABLE
            else:
                state = ComponentHealthState.HEALTHY
            item = ComponentVerification(
                component_id=component_id,
                display_name=manifest_component.name,
                category=manifest_component.category,
                state=state,
                checks=component_checks,
                blocked_by=blocked_by,
                warnings=warnings,
                artifact_ids=planned.artifact_ids,
                build_unit_ids=planned.build_units,
            )
            result_by_id[component_id] = item
            ordered.append(item)
        return tuple(ordered)

    def _activation(self, components: tuple[ComponentVerification, ...], fx_runtime: FxRuntimeIdentity) -> ActivationVerification:
        core = next(item for item in components if item.component_id == "realmheart-core")
        fx = next(item for item in components if item.component_id == "realmheart-fx")
        enabled = self._service_enabled("realmheart.service")
        active = self._service_active("realmheart.service")
        if core.state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED} or fx.state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED}:
            return ActivationVerification(ActivationState.UNKNOWN, RuntimeHealthState.UNKNOWN, "Core/FX install verification failed; runtime activation is not meaningful", enabled, active, fx_runtime)
        if fx_runtime.matches_validated_build is True:
            return ActivationVerification(ActivationState.ACTIVE, RuntimeHealthState.HEALTHY, "running Hyprland attests the exact validated Realmheart FX build", enabled, active, fx_runtime)
        if self.plan.activation.requires_fresh_session_if_unproven:
            return ActivationVerification(ActivationState.PENDING_SESSION_RESTART, RuntimeHealthState.UNKNOWN, fx_runtime.detail, enabled, active, fx_runtime)
        if active:
            return ActivationVerification(ActivationState.ACTIVE, RuntimeHealthState.DEGRADED, "Realmheart shell service is active but FX build identity is unproven", enabled, active, fx_runtime)
        return ActivationVerification(ActivationState.UNKNOWN, RuntimeHealthState.UNKNOWN, "Realmheart runtime activation could not be proven", enabled, active, fx_runtime)

    def _overall_health(self, components: tuple[ComponentVerification, ...]) -> tuple[InstallHealthState, tuple[str, ...], tuple[str, ...]]:
        blockers: list[str] = []
        warnings: list[str] = []
        for item in components:
            if item.state in {ComponentHealthState.FAILED, ComponentHealthState.BLOCKED}:
                if item.category in {"core", "fx", "essential"}:
                    blockers.append(f"{item.display_name}: {item.state.value}")
                else:
                    warnings.append(f"{item.display_name}: {item.state.value}")
            elif item.state is ComponentHealthState.DEGRADED:
                warnings.append(f"{item.display_name}: degraded")
        if blockers:
            return InstallHealthState.FAILED, tuple(blockers), tuple(warnings)
        if warnings:
            return InstallHealthState.DEGRADED, (), tuple(warnings)
        return InstallHealthState.HEALTHY, (), ()

    def _receipt_inputs(
        self,
        *,
        install_health: InstallHealthState,
        activation: ActivationVerification,
        components: tuple[ComponentVerification, ...],
        artifacts: tuple[ObservedArtifactIdentity, ...],
        dependencies: tuple[ObservedDependency, ...],
    ) -> ReceiptInputAssembly:
        component_state = {item.component_id: item.state.value for item in components}
        artifact_by_id = {item.artifact_id: item for item in artifacts}
        build_units: list[ReceiptBuildUnitInput] = []
        for unit in self.plan.build_units:
            states = [component_state.get(cid, ComponentHealthState.NOT_APPLICABLE.value) for cid in unit.component_ids]
            if any(state in {ComponentHealthState.FAILED.value, ComponentHealthState.BLOCKED.value} for state in states):
                health = "failed"
            elif any(state == ComponentHealthState.DEGRADED.value for state in states):
                health = "degraded"
            else:
                health = "healthy"
            hashes = tuple(
                (artifact_id, artifact_by_id[artifact_id].sha256)
                for artifact_id in unit.artifact_ids
                if artifact_id in artifact_by_id and artifact_by_id[artifact_id].sha256 is not None
            )
            build_units.append(ReceiptBuildUnitInput(
                build_unit_id=unit.id,
                component_ids=unit.component_ids,
                health=health,
                artifact_ids=unit.artifact_ids,
                artifact_sha256=hashes,
                abi_sensitive_dependencies=unit.abi_sensitive_dependencies,
            ))
        provenance = asdict(self.build_report.provenance) if self.build_report and self.build_report.provenance else None
        fx_plugin = artifact_by_id.get(self.plan.fx_plan.plugin_artifact_id)
        fx_loader = artifact_by_id.get(self.plan.fx_plan.loader_artifact_id)
        fx_triggers = tuple(
            FxRebuildTriggerInput(
                capability_id=capability_id,
                build_unit_id=self.plan.fx_plan.build_unit,
                trigger="version_commit_or_abi_change",
                observed_version=self.plan.fx_plan.hyprland_version,
                observed_commit=self.plan.fx_plan.hyprland_commit,
                observed_abi_hash=self.plan.fx_plan.hyprland_abi_hash,
            )
            for capability_id in self.plan.fx_plan.rebuild_on_dependency_change
        )
        fx_receipt = FxReceiptInput(
            required=self.plan.fx_plan.required,
            compatibility=self.plan.fx_plan.compatibility.value,
            build_unit_id=self.plan.fx_plan.build_unit,
            build_id=(self.build_report.provenance.fx_build_id if self.build_report and self.build_report.provenance and self.build_report.provenance.fx_build_id else self.plan.fx_plan.build_id),
            plugin_artifact_id=self.plan.fx_plan.plugin_artifact_id,
            loader_artifact_id=self.plan.fx_plan.loader_artifact_id,
            plugin_sha256=fx_plugin.sha256 if fx_plugin else None,
            loader_path=fx_loader.path if fx_loader and fx_loader.exists else None,
            hyprland_version=self.plan.fx_plan.hyprland_version,
            hyprland_commit=self.plan.fx_plan.hyprland_commit,
            hyprland_abi_hash=self.plan.fx_plan.hyprland_abi_hash,
            rebuild_triggers=fx_triggers,
        )
        return ReceiptInputAssembly(
            schema_version=INSTALLED_STATE_SCHEMA_VERSION,
            realmheart_version=self.plan.target_version,
            manifest_schema_version=self.plan.manifest_schema_version,
            manifest_set_sha256=self.plan.manifest_digest,
            installer_version=INSTALLER_VERSION,
            transaction_id=self.plan.transaction_id,
            install_health=install_health.value,
            activation_state=activation.state.value,
            runtime_health=activation.runtime_health.value,
            verified_at=self.verified_at.astimezone(timezone.utc).isoformat(),
            components=components,
            dependencies=dependencies,
            artifacts=artifacts,
            build_units=tuple(build_units),
            fx=fx_receipt,
            build_provenance=provenance,
        )

    # ---- helpers ------------------------------------------------------------

    def _check(self, id: str, component_id: str, check_class: VerificationClass, state: VerificationCheckState, summary: str, *, artifact_id: str | None = None, capability_id: str | None = None, observed: str | None = None, expected: str | None = None, critical: bool = False) -> VerificationCheckResult:
        return VerificationCheckResult(id, component_id, check_class, state, summary, artifact_id, capability_id, observed, expected, critical)

    def _critical(self, component_id: str) -> bool:
        component = self.registry.components.get(component_id)
        return bool(component and component.category in {"core", "fx"})

    def _artifact_target(self, artifact_id: str) -> str | None:
        for item in self.plan.artifact_actions:
            if item.artifact_id == artifact_id:
                return item.target
        return None

    def _capability_executable(self, capability_id: str) -> str | None:
        for item in self.plan.environment.capabilities:
            if item.capability_id == capability_id and item.executable:
                return item.executable
        return None

    def _service_enabled(self, service: str) -> bool | None:
        systemctl = self._capability_executable("runtime.systemctl") or self.runner.which("systemctl")
        if not systemctl:
            return None
        result = self.runner.run((systemctl, "--user", "is-enabled", service), timeout=4.0)
        if result.ok:
            return result.stdout.strip() in {"enabled", "enabled-runtime", "static", "indirect", "generated", "linked", "linked-runtime"}
        if result.stdout.strip() in {"disabled", "masked"} or result.stderr.strip():
            return False
        return None

    def _service_active(self, service: str) -> bool | None:
        systemctl = self._capability_executable("runtime.systemctl") or self.runner.which("systemctl")
        if not systemctl:
            return None
        result = self.runner.run((systemctl, "--user", "is-active", service), timeout=4.0)
        if result.ok:
            return result.stdout.strip() == "active"
        if result.stdout.strip() in {"inactive", "failed", "activating", "deactivating"} or result.stderr.strip():
            return False
        return None


def _sha256_file(path: Path) -> str | None:
    try:
        hasher = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                hasher.update(chunk)
        return hasher.hexdigest()
    except OSError:
        return None


def _command_summary(result) -> str:
    text = (result.stderr or result.stdout or f"exit {result.returncode}").strip().replace("\n", " ")
    return text[:280]


def _parse_identity_output(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if key in {"build_id", "realmheart_version", "hyprland_commit", "hyprland_abi"}:
            values[key] = value.strip()
    return values
