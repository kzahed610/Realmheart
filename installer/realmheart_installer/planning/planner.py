"""Phase-9 installation planner.

The planner is observational.  It turns the validated environment + canonical
manifest into one immutable InstallationPlan.  Dry-run renders this object; the
future live executor must consume the same object instead of re-deriving intent.
"""

from __future__ import annotations

import hashlib
import json
import os
import stat
from pathlib import Path
from typing import Iterable

from realmheart_maintenance.manifest import ManifestError, ManifestRegistry

from ..components.bindings import InstallerBindingRegistry, default_installer_bindings
from ..context import XdgPaths
from ..environment.capabilities import RequirementLevel
from ..environment.installation import InstallOrigin
from ..environment.preflight import EnvironmentSnapshot, PreflightState
from ..environment.support import HyprlandCompatibility
from ..filesystem.backup import validate_backup_snapshot
from ..filesystem.compare import fingerprint_path
from ..errors import PlanningInspectionError
from ..models import FxCompatibility, InstallMode, Reversibility, to_jsonable
from ..package_manager.base import (
    CapabilityProviderPlan,
    DependencyPackagePlan,
    ProviderResolution,
)
from ..package_manager.pacman import PacmanAdapter, build_pacman_dependency_plan, package_required_by
from .models import (
    ActivationPlan,
    ArtifactAction,
    ArtifactCommitClass,
    BackupAction,
    BackupKind,
    BackupTarget,
    BuildPlan,
    BuildVerification,
    ConfigAction,
    ConfigActionKind,
    DiskEstimate,
    FxPlan,
    InstallLayout,
    InstallationPlan,
    PackageAction,
    PackageActionKind,
    PlannedBuildUnit,
    PlannedComponent,
    PlannedHealthCheck,
    PlanState,
    PrivilegedAction,
    ServiceAction,
    ServiceActionKind,
)

PLAN_SCHEMA_VERSION = 2
DEFAULT_PREFIX = Path("/usr/local")
DEFAULT_SYSCONF = Path("/etc")


class InstallationPlanner:
    def __init__(
        self,
        *,
        paths: XdgPaths,
        source_root: Path,
        snapshot: EnvironmentSnapshot,
        registry: ManifestRegistry,
        runner,
        transaction_id: str,
        bindings: InstallerBindingRegistry | None = None,
        prefix: Path = DEFAULT_PREFIX,
        sysconf: Path = DEFAULT_SYSCONF,
        pacman_adapter: PacmanAdapter | None = None,
    ) -> None:
        self.paths = paths
        self.source_root = Path(source_root)
        self.snapshot = snapshot
        self.registry = registry
        self.runner = runner
        self.transaction_id = transaction_id
        self.bindings = bindings or default_installer_bindings()
        self.prefix = Path(prefix)
        self.sysconf = Path(sysconf)
        self.libexec = self.prefix / "libexec"
        self.pacman_adapter = pacman_adapter

    def build(self) -> InstallationPlan:
        blockers: list[str] = []
        warnings = list(self.snapshot.warnings)

        installation = self.snapshot.installation
        if installation.mode is None or installation.source.version_text is None:
            # A plan without a proven mode/target version cannot ever be the
            # authoritative input to a live executor.
            blockers.append("install mode/target version is not provable")
            mode = installation.mode or InstallMode.FRESH
            target_version = installation.source.version_text or self.registry.release_version
        else:
            mode = installation.mode
            target_version = installation.source.version_text

        if self.snapshot.manifest.digest != self.registry.digest:
            blockers.append("preflight manifest digest does not match planner registry")
        if self.registry.release_version != target_version:
            blockers.append(
                f"manifest release {self.registry.release_version} disagrees with target version {target_version}"
            )

        try:
            self.bindings.validate(self.registry)
        except ManifestError as exc:
            blockers.append(str(exc))

        package_plan = self._package_plan()
        package_actions = self._package_actions(package_plan)
        blockers.extend(package_plan.mutation_blockers)
        blockers.extend(self._unresolved_package_blockers(package_plan))
        blockers.extend(self._non_dependency_preflight_blockers(package_plan))

        fx_plan = self._fx_plan()
        if fx_plan.compatibility is FxCompatibility.UNKNOWN:
            blockers.append("required Realmheart FX compatibility is unknown for the running Hyprland version")
        elif fx_plan.compatibility is FxCompatibility.INCOMPATIBLE:
            blockers.append("required Realmheart FX is incompatible with the running Hyprland version")

        config_actions = self._config_actions(fx_plan)
        artifact_actions = self._artifact_actions()
        backup_actions, backup_blockers = self._backup_actions(mode, config_actions, artifact_actions)
        blockers.extend(backup_blockers)

        components = self._components(package_plan)
        build_units = tuple(
            PlannedBuildUnit(
                unit.id,
                unit.cmake_target,
                unit.component_ids,
                unit.artifact_ids,
                unit.build_dependencies,
                unit.abi_sensitive_dependencies,
                unit.rebuild_on_dependency_change,
            )
            for unit in self.registry.build_units.values()
        )
        privileged_actions = self._privileged_actions()
        service_actions = self._service_actions()
        health_checks = tuple(
            PlannedHealthCheck(
                check.id,
                check.component_id,
                check.check,
                check.artifact_id,
                check.cost,
                check.side_effects,
                check.timeout_ms,
                check.contexts,
            )
            for check in self.registry.health_checks.values()
            if "install_verify" in check.contexts
        )

        build = self._build_plan(fx_plan)
        activation = ActivationPlan(
            expected_state="pending_session_restart_if_current_session_cannot_prove_new_hypr_fx",
            reason="install-time health is separate from Hyprland-session activation; a fresh session may be required before runtime promotion",
            requires_fresh_session_if_unproven=True,
        )
        disk = self._disk_estimate(backup_actions, config_actions)
        if disk.known_minimum_fits is False:
            blockers.append(
                f"known minimum backup/config staging requirement ({disk.known_minimum_bytes} bytes) exceeds free space at the config root"
            )
        if not disk.complete:
            warnings.append("disk estimate is a known minimum only; native build and DESTDIR staged payload size are determined in Phase 10")
        if mode is InstallMode.DOWNGRADE:
            warnings.append("downgrade selected: newer Realmheart state/configuration may not be compatible with the older target release")

        # De-duplicate while preserving diagnostic order.
        warnings_tuple = tuple(dict.fromkeys(warnings))
        blockers_tuple = tuple(dict.fromkeys(blockers))
        state = PlanState.BLOCKED if blockers_tuple else PlanState.READY
        layout = InstallLayout(
            prefix=str(self.prefix),
            libexec=str(self.libexec),
            sysconf=str(self.sysconf),
            home=str(self.paths.home),
            xdg_config_home=str(self.paths.config_home),
            xdg_state_home=str(self.paths.state_home),
        )

        plan_kwargs = dict(
            schema_version=PLAN_SCHEMA_VERSION,
            transaction_id=self.transaction_id,
            mode=mode,
            current_version=installation.installed_version_text,
            target_version=target_version,
            source_revision=installation.source.git_commit,
            source_dirty=installation.source.git_dirty,
            environment=self.snapshot,
            manifest_digest=self.registry.digest,
            manifest_schema_version=self.registry.schema_version,
            package_plan=package_plan,
            package_actions=package_actions,
            backup_actions=backup_actions,
            config_actions=config_actions,
            artifact_actions=artifact_actions,
            components=components,
            build_units=build_units,
            fx_plan=fx_plan,
            privileged_actions=privileged_actions,
            service_actions=service_actions,
            health_checks=health_checks,
            build=build,
            activation=activation,
            disk=disk,
            layout=layout,
            warnings=warnings_tuple,
            blockers=blockers_tuple,
            state=state,
        )
        digest = self._plan_digest(plan_kwargs)
        return InstallationPlan(**plan_kwargs, plan_digest=digest)

    def _package_plan(self) -> DependencyPackagePlan:
        missing = tuple(
            result for result in self.snapshot.capabilities
            if not result.satisfied and result.requirement is not RequirementLevel.SOFT
        )
        manager = self.snapshot.package_manager.kind or "none"
        if not missing:
            return DependencyPackagePlan(manager, (), (), ())
        if manager == "pacman" and self.snapshot.package_manager.automatic_dependency_install:
            adapter = self.pacman_adapter or PacmanAdapter(self.runner)
            return build_pacman_dependency_plan(self.snapshot.capabilities, adapter, registry=self.registry)
        providers = tuple(
            CapabilityProviderPlan(
                item.capability_id,
                item.display_name,
                (),
                ProviderResolution.UNMAPPED,
                "automatic dependency installation is unavailable for this distribution/package manager",
                item.component,
            )
            for item in missing
        )
        return DependencyPackagePlan(manager, providers, (), ())

    def _package_actions(self, plan: DependencyPackagePlan) -> tuple[PackageAction, ...]:
        if not plan.packages:
            return ()
        adapter = self.pacman_adapter or PacmanAdapter(self.runner)
        required_by = package_required_by(plan)
        upgrade_packages = {
            package
            for provider in plan.providers
            if provider.resolution is ProviderResolution.UPGRADE
            for package in provider.packages
        }
        actions: list[PackageAction] = []
        for package in plan.packages:
            state = adapter.query(package)
            actions.append(
                PackageAction(
                    package=package,
                    action=PackageActionKind.UPGRADE if package in upgrade_packages else PackageActionKind.INSTALL,
                    installed_version=state.installed_version,
                    repository_version=state.repository_version,
                    required_by=required_by.get(package, ()),
                )
            )
        return tuple(actions)

    def _unresolved_package_blockers(self, plan: DependencyPackagePlan) -> tuple[str, ...]:
        return tuple(f"unresolved dependency {item.capability_id}: {item.reason}" for item in plan.unresolved)

    def _non_dependency_preflight_blockers(self, package_plan: DependencyPackagePlan) -> tuple[str, ...]:
        # Missing capabilities are reclassified by the package plan.  Everything
        # else discovered by preflight remains a planner blocker.
        dependency_ids = {
            item.capability_id
            for item in self.snapshot.capabilities
            if not item.satisfied and item.requirement is not RequirementLevel.SOFT
        }
        removable = {f"missing dependency capability: {capability_id}" for capability_id in dependency_ids}
        blockers = [item for item in self.snapshot.blockers if item not in removable]

        if self.snapshot.state in {
            PreflightState.SOURCE_INVALID,
            PreflightState.INSTALL_STATE_CONFLICT,
            PreflightState.UNSUPPORTED_ENVIRONMENT,
            PreflightState.UNKNOWN_HYPRLAND,
        }:
            # Preserve state even in the unlikely event that its human blocker
            # list is incomplete.
            marker = f"preflight state is {self.snapshot.state.value}"
            if marker not in blockers:
                blockers.append(marker)
        return tuple(blockers)

    def _fx_plan(self) -> FxPlan:
        compatibility = self.snapshot.hyprland.compatibility
        if compatibility in {HyprlandCompatibility.PREFERRED, HyprlandCompatibility.SUPPORTED}:
            if not self.snapshot.hyprland.commit or not self.snapshot.hyprland.abi_hash or self.snapshot.hyprland.dirty is not False:
                state = FxCompatibility.UNKNOWN
                action = "block_supported_install"
                reason = "Hyprland version is supported, but its exact clean commit/ABI identity is unavailable; required FX cannot be built safely"
            else:
                state = FxCompatibility.COMPATIBLE
                action = "build_stage_install_required"
                reason = "running Hyprland is within Realmheart's supported/tested FX policy with a complete clean commit/ABI identity"
        elif compatibility in {HyprlandCompatibility.UNKNOWN, HyprlandCompatibility.UNPARSEABLE}:
            state = FxCompatibility.UNKNOWN
            action = "block_supported_install"
            reason = "Hyprland/FX ABI compatibility has not been validated for this version"
        else:
            state = FxCompatibility.INCOMPATIBLE
            action = "block_supported_install"
            reason = "running Hyprland does not satisfy Realmheart's required FX policy"
        build_unit = self.registry.build_units.get("realmheart-fx")
        rebuild = tuple(build_unit.rebuild_on_dependency_change) if build_unit else ("dep.hyprland.devel",)
        identity_material = "\0".join((
            self.transaction_id,
            self.registry.digest,
            self.snapshot.installation.source.git_commit or "unknown-source",
            "dirty" if self.snapshot.installation.source.git_dirty else "clean",
            self.snapshot.hyprland.commit or "unknown-hyprland-commit",
            self.snapshot.hyprland.abi_hash or "unknown-hyprland-abi",
        ))
        build_id = hashlib.sha256(identity_material.encode("utf-8")).hexdigest()
        return FxPlan(
            required=True,
            compatibility=state,
            action=action,
            reason=reason,
            hyprland_version=str(self.snapshot.hyprland.version) if self.snapshot.hyprland.version else self.snapshot.hyprland.raw_version,
            hyprland_commit=self.snapshot.hyprland.commit,
            hyprland_abi_hash=self.snapshot.hyprland.abi_hash,
            build_unit="realmheart-fx",
            build_id=build_id,
            plugin_artifact_id="fx.plugin",
            loader_artifact_id="fx.loader",
            rebuild_on_dependency_change=rebuild,
        )

    def _config_actions(self, fx_plan: FxPlan) -> tuple[ConfigAction, ...]:
        actions: list[ConfigAction] = []

        hypr_target = self.paths.config_home / "hypr"
        actions.append(ConfigAction(
            id="config.hypr.takeover",
            component_id="hypr-integration",
            kind=ConfigActionKind.FULL_TREE_REPLACE,
            target=str(hypr_target),
            source=str(self.source_root / "config/hypr"),
            will_mutate=True,
            reason="staged full-tree Realmheart Hyprland integration; current custom/ is overlaid as a preservation island",
            backup_policy="baseline_or_pre_adoption_plus_transaction_preimage",
            reversibility=Reversibility.EXACT,
            precondition_fingerprint=self._fingerprint(hypr_target),
            preserve=(str(hypr_target / "custom"),),
        ))

        kitty_conf = self.paths.config_home / "kitty/kitty.conf"
        actions.append(ConfigAction(
            id="config.kitty.managed-block",
            component_id="terminal",
            kind=ConfigActionKind.MANAGED_BLOCK,
            target=str(kitty_conf),
            source=None,
            will_mutate=True,
            reason="ensure exactly one Realmheart Terminal include block while preserving unrelated Kitty configuration",
            backup_policy="transaction_preimage",
            reversibility=Reversibility.GUARDED,
            precondition_fingerprint=self._fingerprint(kitty_conf),
            render_strategy="kitty-managed-include-v1",
            render_values=(("INCLUDE_PATH", str(self.paths.config_home / "kitty/realmheart-theme.conf")),),
        ))

        fish_config = self.paths.config_home / "fish/config.fish"
        actions.append(ConfigAction(
            id="config.fish.personal",
            component_id="terminal",
            kind=ConfigActionKind.READ_ONLY,
            target=str(fish_config),
            source=None,
            will_mutate=False,
            reason="personal Fish config is intentionally untouched; Realmheart uses conf.d drop-ins",
            backup_policy="none",
            reversibility=Reversibility.NONE,
            precondition_fingerprint=self._fingerprint(fish_config),
        ))

        # Realmheart-owned source-backed user artifacts are path-specific.  The
        # shared ~/.config/realmheart namespace is expanded into an allowlist;
        # it is never recursively replaced.
        for artifact in self.registry.artifacts.values():
            if artifact.id in {"hypr.tree", "realmheart.config", "terminal.generated-starship"}:
                continue
            if artifact.ownership not in {"user", "shared"} or not artifact.source:
                continue
            target = Path(self._expand_path(artifact.path))
            source = self.source_root / artifact.source
            needs_template_render = artifact.id in {"terminal.kitty-dropin", "fx.loader"} or artifact.source.endswith(".in") or (
                artifact.type == "service" and (
                    self.paths.config_home != self.paths.home / ".config"
                    or self.paths.state_home != self.paths.home / ".local/state"
                )
            )
            kind = ConfigActionKind.RENDERED_FILE if needs_template_render else ConfigActionKind.OWNED_FILE
            will_mutate = True
            reversibility = Reversibility.EXACT
            backup_policy = "transaction_preimage"
            render_strategy = None
            render_values: tuple[tuple[str, str], ...] = ()
            if artifact.id == "terminal.kitty-dropin":
                render_strategy = "terminal-kitty-dropin-v1"
                render_values = (("STATE_THEME", str(self.paths.state_home / "realmheart/theme/kitty-theme.conf")),)
            elif artifact.id == "fx.loader":
                render_strategy = "realmheart-fx-loader-v1"
                render_values = (
                    ("@REALMHEART_FX_HYPRLAND_COMMIT@", fx_plan.hyprland_commit or ""),
                    ("@REALMHEART_FX_HYPRLAND_ABI@", fx_plan.hyprland_abi_hash or ""),
                    ("@REALMHEART_FX_BUILD_ID@", fx_plan.build_id),
                    ("@REALMHEART_FX_PLUGIN_PATH@", str(self.prefix / "lib/realmheart/realmheart-fx.so")),
                    ("@REALMHEART_PLUGIN_RELATIVE_DIR@", "../lib/realmheart"),
                )
            elif artifact.id in {"clipboard.text-service", "clipboard.image-service"}:
                render_strategy = "token-substitution-v1"
                render_values = (
                    ("@WL_PASTE@", self._capability_executable("runtime.wl-paste", "wl-paste")),
                    ("@CLIPHIST@", self._capability_executable("runtime.cliphist", "cliphist")),
                )
            elif artifact.id == "doctor.boot-service":
                render_strategy = "token-substitution-v1"
                render_values = (
                    ("@REALMHEART_DOCTOR_BIN@", str(self.prefix / "bin" / "realmheart-doctor")),
                    ("@REALMHEART_DOCTOR_STATE_DIR@", str(self.paths.state_home / "realmheart" / "doctor")),
                )
            elif needs_template_render and artifact.id in {"terminal.theme-service", "terminal.theme-path"}:
                render_strategy = "rewrite-default-xdg-v1"
                render_values = (
                    ("%h/.config", str(self.paths.config_home)),
                    ("%h/.local/state", str(self.paths.state_home)),
                )
            reason = (
                "render trusted service source with resolved executable/XDG paths, then install atomically"
                if needs_template_render else
                "install/update Realmheart-owned user artifact"
            )
            actions.append(ConfigAction(
                id=f"config.artifact.{artifact.id}",
                component_id=artifact.component_id,
                kind=kind,
                target=str(target),
                source=str(source),
                will_mutate=will_mutate,
                reason=reason,
                backup_policy=backup_policy,
                reversibility=reversibility,
                precondition_fingerprint=self._fingerprint(target),
                render_strategy=render_strategy,
                render_values=render_values,
                mode=artifact.mode,
            ))

        # Source-less user artifacts are rendered from a stable installer
        # contract. The renderer identity and resolved values live in the plan,
        # so the live executor cannot silently re-decide their content later.
        generated_contracts = (
            (
                "core.service",
                "realmheart-core-user-service-v1",
                (("REALMHEART_BINARY", str(self.prefix / "bin/realmheart")),
                 ("ARGS", "--shell --wallpaper-backend native")),
                "render Realmheart shell user service against the installed prefix",
            ),
            (
                "event.service",
                "realmheart-eventd-user-service-v1",
                (("REALMHEART_EVENTD_BINARY", str(self.prefix / "bin/realmheart-eventd")),),
                "render Event Surface user service against the installed prefix",
            ),
            (
                "auth.lock-session",
                "realmheart-lock-session-v1",
                (("REALMHEART_BINARY", str(self.prefix / "bin/realmheart")),
                 ("SYSTEMCTL", self._capability_executable("runtime.systemctl", "systemctl")),
                 ("LOGINCTL", self._capability_executable("runtime.loginctl", "loginctl"))),
                "render lock-session wrapper bound to the installed Realmheart binary and probed session tools",
            ),
        )
        for artifact_id, strategy, values, reason in generated_contracts:
            artifact = self.registry.artifacts.get(artifact_id)
            if not artifact:
                continue
            target = Path(self._expand_path(artifact.path))
            actions.append(ConfigAction(
                id=f"config.generated.{artifact.id}",
                component_id=artifact.component_id,
                kind=ConfigActionKind.RENDERED_FILE,
                target=str(target),
                source=None,
                will_mutate=True,
                reason=reason,
                backup_policy="transaction_preimage",
                reversibility=Reversibility.EXACT,
                precondition_fingerprint=self._fingerprint(target),
                render_strategy=strategy,
                render_values=values,
            ))

        shared = self.registry.artifacts.get("realmheart.config")
        if shared and shared.source:
            source_root = self.source_root / shared.source
            target_root = Path(self._expand_path(shared.path))
            if source_root.is_dir():
                for source in _regular_source_files(source_root):
                    relative = source.relative_to(source_root)
                    target = target_root / relative
                    exists = target.exists() or target.is_symlink()
                    actions.append(ConfigAction(
                        id="config.realmheart.seed." + ".".join(relative.parts),
                        component_id=shared.component_id,
                        kind=ConfigActionKind.SHARED_SEED,
                        target=str(target),
                        source=str(source),
                        will_mutate=not exists,
                        reason=(
                            "seed missing Realmheart config artifact without overwriting existing user-managed state"
                            if not exists else
                            "existing user-managed Realmheart config artifact is preserved"
                        ),
                        backup_policy="transaction_preimage_if_created" if not exists else "none",
                        reversibility=Reversibility.GUARDED if not exists else Reversibility.NONE,
                        precondition_fingerprint=self._fingerprint(target),
                    ))

        generated = self.registry.artifacts.get("terminal.generated-starship")
        if generated:
            target = self.paths.state_home / "realmheart/theme"
            actions.append(ConfigAction(
                id="config.terminal.generated-state",
                component_id=generated.component_id,
                kind=ConfigActionKind.GENERATED_STATE,
                target=str(target),
                source=None,
                will_mutate=True,
                reason="run the installed terminal generator on the target machine; generated theme state is not copied from the developer checkout",
                backup_policy="generated_state_regenerable",
                reversibility=Reversibility.BEST_EFFORT,
                precondition_fingerprint=self._fingerprint(target),
            ))
        return tuple(actions)

    def _artifact_actions(self) -> tuple[ArtifactAction, ...]:
        config_artifact_ids = {
            "hypr.tree", "realmheart.config", "terminal.kitty-dropin", "terminal.fish-theme",
            "terminal.fish-starship", "terminal.generator", "terminal.theme-service",
            "terminal.theme-path", "clipboard.text-service", "clipboard.image-service",
            "terminal.generated-starship",
        }
        actions: list[ArtifactAction] = []
        for artifact in self.registry.artifacts.values():
            target = self._expand_path(artifact.path)
            if artifact.type == "generated":
                commit_class = ArtifactCommitClass.GENERATED
            elif artifact.id in config_artifact_ids:
                commit_class = ArtifactCommitClass.USER_COMMIT
            elif artifact.ownership == "system" or target.startswith(str(self.prefix) + "/") or target.startswith(str(self.sysconf) + "/"):
                commit_class = ArtifactCommitClass.PRIVILEGED_COMMIT
            elif artifact.ownership in {"user", "shared"}:
                commit_class = ArtifactCommitClass.USER_COMMIT
            else:
                commit_class = ArtifactCommitClass.STAGED_PAYLOAD
            privileged = commit_class is ArtifactCommitClass.PRIVILEGED_COMMIT
            source = str(self.source_root / artifact.source) if artifact.source else None
            actions.append(ArtifactAction(
                artifact.id,
                artifact.component_id,
                target,
                artifact.type,
                artifact.ownership,
                commit_class,
                artifact.required,
                source,
                privileged,
            ))
        return tuple(actions)

    def _components(self, package_plan: DependencyPackagePlan) -> tuple[PlannedComponent, ...]:
        unresolved_ids = {item.capability_id for item in package_plan.unresolved}
        package_resolved_ids = {
            item.capability_id for item in package_plan.providers
            if item.resolution in {ProviderResolution.INSTALL, ProviderResolution.UPGRADE}
        }
        capabilities_by_id = {item.capability_id: item for item in self.snapshot.capabilities}
        result: list[PlannedComponent] = []
        for order, component_id in enumerate(self.registry.component_order, start=1):
            component = self.registry.components[component_id]
            capability_ids = tuple(spec.id for spec in self.registry.component_capabilities(component_id))
            reasons: list[str] = []
            state = "ready"
            for capability_id in capability_ids:
                observed = capabilities_by_id.get(capability_id)
                if observed is None or observed.satisfied or observed.requirement is RequirementLevel.SOFT:
                    continue
                if capability_id in package_resolved_ids:
                    state = "pending_package"
                    reasons.append(f"{capability_id} will be provided/upgraded by package action")
                elif capability_id in unresolved_ids:
                    state = "blocked"
                    reasons.append(f"{capability_id} requires manual/unavailable dependency remediation")
            artifact_ids = tuple(a.id for a in self.registry.artifacts.values() if a.component_id == component_id)
            health_ids = tuple(h.id for h in self.registry.health_checks.values() if h.component_id == component_id)
            result.append(PlannedComponent(
                order,
                component.id,
                component.name,
                component.category,
                component.stage,
                self.registry.release_version if component.component_version == "release" else component.component_version,
                tuple(dep.id for dep in component.realmheart_dependencies if dep.required),
                capability_ids,
                component.build_units,
                artifact_ids,
                health_ids,
                component.requires_installer_binding,
                state,
                tuple(reasons),
            ))
        return tuple(result)

    def _backup_actions(
        self,
        mode: InstallMode,
        config_actions: tuple[ConfigAction, ...],
        artifact_actions: tuple[ArtifactAction, ...],
    ) -> tuple[tuple[BackupAction, ...], tuple[str, ...]]:
        blockers: list[str] = []
        targets = self._backup_targets(config_actions, artifact_actions)
        actions: list[BackupAction] = []
        baseline_exists = self.paths.baseline_backup.exists() or self.paths.baseline_backup.is_symlink()

        if self.snapshot.installation.requires_pre_adoption_snapshot:
            destination = self.paths.version_backups / f"pre-adoption-{self.transaction_id}"
            actions.append(BackupAction(
                "backup.pre-adoption",
                BackupKind.PRE_ADOPTION,
                str(destination),
                True,
                destination.exists(),
                "adopt unmanaged/legacy Realmheart conservatively without pretending historical .bak files are pristine",
                targets,
            ))
        elif mode is InstallMode.FRESH:
            if baseline_exists:
                validation = validate_backup_snapshot(self.paths.baseline_backup)
                if not validation.valid:
                    blockers.append("existing permanent baseline is invalid: " + "; ".join(validation.errors))
                actions.append(BackupAction(
                    "backup.baseline.preserve",
                    BackupKind.PRESERVE_EXISTING_BASELINE,
                    str(self.paths.baseline_backup),
                    True,
                    True,
                    "existing permanent baseline will be validated and preserved; it is never overwritten",
                    (),
                ))
            else:
                actions.append(BackupAction(
                    "backup.baseline.create",
                    BackupKind.PERMANENT_BASELINE,
                    str(self.paths.baseline_backup),
                    True,
                    False,
                    "capture permanent pre-Realmheart state before the first managed mutation",
                    targets,
                ))
        else:
            if baseline_exists:
                validation = validate_backup_snapshot(self.paths.baseline_backup)
                if not validation.valid:
                    blockers.append("existing permanent baseline is invalid: " + "; ".join(validation.errors))
                actions.append(BackupAction(
                    "backup.baseline.preserve",
                    BackupKind.PRESERVE_EXISTING_BASELINE,
                    str(self.paths.baseline_backup),
                    True,
                    True,
                    "permanent pre-Realmheart baseline remains immutable across reinstall/upgrade/downgrade",
                    (),
                ))
            elif self.snapshot.installation.origin is InstallOrigin.MANAGED_INSTALLER:
                blockers.append("managed Realmheart installation is missing its permanent pre-Realmheart baseline; refusing to invent a replacement from already-managed state")
            destination = self.paths.version_backups / f"realmheart-{self.snapshot.installation.installed_version_text or 'unknown'}-{self.transaction_id}"
            actions.append(BackupAction(
                "backup.previous-version",
                BackupKind.PREVIOUS_VERSION,
                str(destination),
                True,
                destination.exists(),
                "capture the current Realmheart installation as the immediate rollback target",
                targets,
            ))
        return tuple(actions), tuple(blockers)

    def _backup_targets(
        self,
        config_actions: tuple[ConfigAction, ...],
        artifact_actions: tuple[ArtifactAction, ...],
    ) -> tuple[BackupTarget, ...]:
        targets: dict[str, BackupTarget] = {}
        for action in config_actions:
            if not action.will_mutate or action.kind is ConfigActionKind.GENERATED_STATE:
                continue
            path = Path(action.target)
            targets[action.target] = self._backup_target(action.id, path, privileged=False)
        for action in artifact_actions:
            if not action.required:
                continue
            if action.commit_class not in {ArtifactCommitClass.PRIVILEGED_COMMIT, ArtifactCommitClass.STAGED_PAYLOAD}:
                continue
            path = Path(action.target)
            targets[action.target] = self._backup_target(action.artifact_id, path, privileged=action.privileged)
        return tuple(targets[key] for key in sorted(targets))

    def _backup_target(self, label: str, path: Path, *, privileged: bool) -> BackupTarget:
        exists = path.exists() or path.is_symlink()
        size = _path_size(path) if exists else 0
        return BackupTarget(label, str(path), self._fingerprint(path), exists, size, privileged)

    def _privileged_actions(self) -> tuple[PrivilegedAction, ...]:
        result: list[PrivilegedAction] = []
        helper = self.registry.artifacts.get("auth.helper")
        pam = self.registry.artifacts.get("auth.pam")
        if helper:
            target = Path(self._expand_path(helper.path))
            result.append(PrivilegedAction(
                "privileged.auth-helper",
                helper.component_id,
                str(target),
                "commit staged auth helper and verify regular-file/no-symlink secure path metadata",
                "root", "root", "4755", Reversibility.GUARDED, self._fingerprint(target),
            ))
        if pam:
            target = Path(self._expand_path(pam.path))
            result.append(PrivilegedAction(
                "privileged.pam-service",
                pam.component_id,
                str(target),
                "install Realmheart PAM service from declared source and verify secure ownership/mode",
                "root", "root", "0644", Reversibility.GUARDED, self._fingerprint(target),
            ))
        return tuple(result)

    def _service_actions(self) -> tuple[ServiceAction, ...]:
        service_artifacts = tuple(a for a in self.registry.artifacts.values() if a.type == "service")
        if not service_artifacts:
            return ()
        actions: list[ServiceAction] = [
            ServiceAction(
                "service.daemon-reload",
                "systemd --user",
                None,
                ServiceActionKind.DAEMON_RELOAD,
                "reload user manager after journaled unit-file commits",
            )
        ]
        for artifact in service_artifacts:
            service = Path(self._expand_path(artifact.path)).name
            if service == "realmheart-terminal-theme.service":
                action = ServiceActionKind.INSTALL_ONLY
                reason = "helper service is activated by realmheart-terminal-theme.path"
            elif service == "realmheart.service":
                action = ServiceActionKind.ENABLE_ONLY
                reason = "enable the shell service but defer starting/restarting it until activation can be proven safe for the current Hyprland/FX session"
            elif service == "realmheart-doctor-boot.service":
                action = ServiceActionKind.ENABLE_ONLY
                reason = "enable the Doctor boot one-shot but defer starting it to the next graphical session, where runtime probes see a real compositor"
            else:
                action = ServiceActionKind.ENABLE_START
                reason = "enable and start Realmheart user unit after install verification prerequisites are satisfied"
            actions.append(ServiceAction(f"service.{artifact.id}", service, artifact.component_id, action, reason))
        return tuple(actions)

    def _build_plan(self, fx_plan: FxPlan) -> BuildPlan:
        build_dir = self.paths.installer_cache / "build" / self.transaction_id
        stage_dir = self.paths.installer_cache / "stage" / self.transaction_id
        cmake = self._capability_executable("build.cmake", "cmake")
        ninja = self._capability_executable("build.ninja", "ninja")
        bash = self._capability_executable("runtime.bash", "bash")
        configure = (
            "-S", str(self.source_root),
            "-B", str(build_dir),
            "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={self.prefix}",
            "-DCMAKE_INSTALL_BINDIR=bin",
            "-DCMAKE_INSTALL_LIBDIR=lib",
            "-DCMAKE_INSTALL_LIBEXECDIR=libexec",
            "-DCMAKE_INSTALL_DATADIR=share",
            f"-DCMAKE_INSTALL_SYSCONFDIR={self.sysconf}",
            "-DREALMHEART_EVENTD_AUTOSTART=OFF",
            "-DREALMHEART_BUILD_HYPRLAND_PLUGIN=ON",
            f"-DREALMHEART_FX_BUILD_ID={fx_plan.build_id}",
            f"-DREALMHEART_FX_HYPRLAND_COMMIT={fx_plan.hyprland_commit or ''}",
            f"-DREALMHEART_FX_HYPRLAND_ABI={fx_plan.hyprland_abi_hash or ''}",
            f"-DREALMHEART_FX_PLUGIN_PATH={self.prefix / 'lib/realmheart/realmheart-fx.so'}",
            "-DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON",
            "-DREALMHEART_ENABLE_SCREENSHOT=ON",
            "-DBUILD_TESTING=OFF",
        )
        return BuildPlan(
            build_dir=str(build_dir),
            stage_dir=str(stage_dir),
            generator="Ninja",
            build_type="Release",
            install_prefix=str(self.prefix),
            cmake_executable=cmake,
            ninja_executable=ninja,
            configure_args=configure,
            build_environment=(("REALMHEART_EVENTD_AUTOSTART_DISABLE", "1"),),
            install_environment=(("REALMHEART_EVENTD_AUTOSTART_DISABLE", "1"), ("REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL", "1"), ("DESTDIR", str(stage_dir))),
            source_prerequisites=(
                "CMakeLists.txt",
                "assets",
                "styles",
                "effects",
                "config/pam/realmheart-lockscreen",
                "config/bin/realmheart-fx-load",
            ),
            verification=(
                BuildVerification("eventd-autostart-isolation", (bash, str(self.source_root / "tests/EventDaemonAutostartTests.sh"), str(self.source_root)), 30, (("REALMHEART_EVENTD_AUTOSTART_DISABLE", "0"),)),
                BuildVerification("fx-loader-contract", (bash, str(self.source_root / "tests/RealmheartFxLoaderTests.sh"), str(self.source_root)), 30),
                BuildVerification("lock-routing-contract", (bash, str(self.source_root / "tests/LockRoutingTests.sh"), str(self.source_root)), 30),
                BuildVerification("screenshot-utility-contract", (bash, str(self.source_root / "tests/ScreenshotUtilityTests.sh"), str(self.source_root)), 45),
            ),
            allowed_uncommitted_stage_paths=(str(self.prefix / "bin/realmheart-fx-load"),),
            side_effects_disabled=True,
        )

    def _disk_estimate(self, backups: tuple[BackupAction, ...], config_actions: tuple[ConfigAction, ...]) -> DiskEstimate:
        # Count each live path once even when more than one conceptual safety
        # layer refers to it.
        backup_paths: dict[str, int] = {}
        for action in backups:
            if action.kind is BackupKind.PRESERVE_EXISTING_BASELINE:
                continue
            for target in action.targets:
                if target.exists and target.estimated_bytes is not None:
                    backup_paths[target.path] = target.estimated_bytes
        backup_bytes = sum(backup_paths.values())
        staging_sources: dict[str, int] = {}
        for action in config_actions:
            if action.kind is not ConfigActionKind.FULL_TREE_REPLACE or not action.source:
                continue
            source = Path(action.source)
            staging_sources[str(source)] = _path_size(source) if source.exists() else 0
        config_staging = sum(staging_sources.values())
        known = backup_bytes + config_staging
        free = next((item.free_bytes for item in self.snapshot.filesystem if item.path == str(self.paths.config_home)), None)
        if free is None:
            # Preflight may report a different textual form; pick the lowest
            # known writable target free-space value as a conservative fallback.
            values = [item.free_bytes for item in self.snapshot.filesystem if item.required_writable and item.free_bytes is not None]
            free = min(values) if values else None
        fits = None if free is None else known <= free
        return DiskEstimate(
            backup_bytes=backup_bytes,
            config_staging_bytes=config_staging,
            known_minimum_bytes=known,
            free_bytes_at_config_root=free,
            known_minimum_fits=fits,
            complete=False,
            note="known minimum covers current preimages plus config staging; native build and DESTDIR payload are measured in Phase 10",
        )


    def _fingerprint(self, path: Path) -> str:
        try:
            return fingerprint_path(path)
        except OSError as exc:
            reason = exc.strerror or str(exc)
            raise PlanningInspectionError(str(path), reason) from exc

    def _capability_executable(self, capability_id: str, fallback: str) -> str:
        for result in self.snapshot.capabilities:
            if result.capability_id == capability_id and result.executable:
                return result.executable
        return fallback

    def _expand_path(self, template: str) -> str:
        replacements = {
            "$HOME": str(self.paths.home),
            "$XDG_CONFIG_HOME": str(self.paths.config_home),
            "$XDG_STATE_HOME": str(self.paths.state_home),
            "$PREFIX": str(self.prefix),
            "$LIBEXEC": str(self.libexec),
            "$SYSCONF": str(self.sysconf),
        }
        result = template
        for token in sorted(replacements, key=len, reverse=True):
            result = result.replace(token, replacements[token])
        return result

    @staticmethod
    def _plan_digest(kwargs: dict) -> str:
        payload = json.dumps(to_jsonable(kwargs), sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        return hashlib.sha256(payload).hexdigest()


def _regular_source_files(root: Path) -> tuple[Path, ...]:
    files: list[Path] = []
    for path in sorted(root.rglob("*"), key=lambda item: os.fsencode(str(item.relative_to(root)))):
        if path.is_symlink():
            # A shared namespace must not smuggle a link outside itself.  This
            # is product input; fail closed rather than following it.
            continue
        if path.is_file():
            files.append(path)
    return tuple(files)


def _path_size(path: Path) -> int | None:
    """Symlink-safe apparent-byte estimate for planning only."""
    try:
        st = path.lstat()
    except OSError:
        return None
    if stat.S_ISLNK(st.st_mode):
        try:
            return len(os.fsencode(os.readlink(path)))
        except OSError:
            return None
    if stat.S_ISREG(st.st_mode):
        return st.st_size
    if stat.S_ISDIR(st.st_mode):
        total = 0
        try:
            with os.scandir(path) as iterator:
                entries = list(iterator)
        except OSError:
            return None
        for entry in entries:
            child = _path_size(Path(entry.path))
            if child is None:
                return None
            total += child
        return total
    return 0
