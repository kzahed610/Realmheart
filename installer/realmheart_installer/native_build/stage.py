"""Phase-10 normal-user native build + CMake DESTDIR staging executor.

This module is deliberately limited to installer-private build/cache state.  It
never commits into /usr/local, /etc, user configuration, or systemd state.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
from pathlib import Path

from realmheart_maintenance.manifest import ManifestRegistry, load_manifest

from ..environment.installation import detect_source_identity
from ..filesystem.compare import fingerprint_path
from ..planning.models import ArtifactCommitClass, InstallationPlan, PlanState
from .models import (
    BuildCommandRecord,
    BuildProvenance,
    BuildStageReport,
    BuildStageState,
    BuildUnitResult,
    StagedArtifactResult,
)

REPORT_SCHEMA_VERSION = 1
_TAIL_LIMIT = 6000


class NativeBuildExecutor:
    def __init__(
        self,
        *,
        plan: InstallationPlan,
        source_root: Path,
        registry: ManifestRegistry,
        runner,
        installer_cache: Path,
    ) -> None:
        self.plan = plan
        self.source_root = Path(source_root)
        self.registry = registry
        self.runner = runner
        self.installer_cache = Path(installer_cache)
        self.build_dir = Path(plan.build.build_dir)
        self.stage_dir = Path(plan.build.stage_dir)
        self.commands: list[BuildCommandRecord] = []
        self.warnings: list[str] = []
        self.blockers: list[str] = []
        self.unit_results: list[BuildUnitResult] = []
        self.artifact_results: list[StagedArtifactResult] = []

    def run(self) -> BuildStageReport:
        configured = False
        built = False
        checks = False
        installed = False
        eventd_unit_before = self._eventd_unit_fingerprint()
        eventd_runtime_before = self._eventd_runtime_signature()
        live_before = self._live_target_fingerprints()
        if self.blockers:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        if self.plan.state is not PlanState.READY:
            self.blockers.append("authoritative InstallationPlan is blocked; native build is not allowed")
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)
        if not self.plan.build.side_effects_disabled:
            self.blockers.append("build plan does not prove build-time live side effects are disabled")
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        self._validate_source_identity()
        self._validate_source_prerequisites()
        self._validate_private_paths()
        if self.blockers:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        self._reset_private_directory(self.build_dir)
        self._reset_private_directory(self.stage_dir)
        if self.blockers:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        build_env = dict(self.plan.build.build_environment)
        configure = (self.plan.build.cmake_executable, *self.plan.build.configure_args)
        result = self._command("configure", configure, timeout=180.0, env=build_env)
        if not result.ok:
            self.blockers.append("CMake configure failed")
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)
        configured = True
        self._validate_cmake_cache()
        if self.blockers:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        all_units_ok = True
        for unit in self.plan.build_units:
            if not unit.cmake_target:
                continue
            command = (
                self.plan.build.cmake_executable,
                "--build", str(self.build_dir),
                "--target", unit.cmake_target,
                "--parallel",
            )
            target_result = self._command(f"build:{unit.id}", command, timeout=3600.0, env=build_env)
            ok = target_result.ok
            reason = None if ok else f"CMake target {unit.cmake_target} failed"
            self.unit_results.append(BuildUnitResult(unit.id, unit.cmake_target, ok, unit.artifact_ids, reason))
            if not ok:
                all_units_ok = False
                if unit.id == self.plan.fx_plan.build_unit:
                    self.blockers.append("required Realmheart FX build target failed")
                else:
                    self.blockers.append(reason)
                break
        built = all_units_ok and len(self.unit_results) == len([u for u in self.plan.build_units if u.cmake_target])
        if not built:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        checks = self._run_self_checks(build_env)
        if not checks:
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

        install_env = dict(build_env)
        install_env.update(dict(self.plan.build.install_environment))
        install_command = (
            self.plan.build.cmake_executable,
            "--install", str(self.build_dir),
            "--prefix", self.plan.build.install_prefix,
        )
        install_result = self._command("destdir-install", install_command, timeout=900.0, env=install_env)
        if not install_result.ok:
            self.blockers.append("unprivileged CMake DESTDIR install failed")
            return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)
        installed = True

        self._validate_staged_payload()
        self._validate_live_targets(live_before)
        self._validate_eventd_side_effects(eventd_unit_before, eventd_runtime_before)
        return self._report(configured, built, checks, installed, eventd_unit_before, eventd_runtime_before)

    def _validate_source_identity(self) -> None:
        source = detect_source_identity(self.source_root, self.runner)
        if source.error:
            self.blockers.append(source.error)
            return
        if source.version_text != self.plan.target_version:
            self.blockers.append(
                f"source version drifted after planning: planned {self.plan.target_version}, observed {source.version_text}"
            )
        if self.plan.source_revision and source.git_commit and source.git_commit != self.plan.source_revision:
            self.blockers.append(
                f"source revision drifted after planning: planned {self.plan.source_revision}, observed {source.git_commit}"
            )
        if self.plan.source_dirty is not None and source.git_dirty is not None and source.git_dirty != self.plan.source_dirty:
            self.blockers.append("source dirty/clean state changed after planning; rebuild the authoritative plan")
        try:
            current_registry = load_manifest(self.source_root / "components")
        except Exception as exc:
            self.blockers.append(f"canonical manifest cannot be reloaded before build: {exc}")
            return
        if current_registry.digest != self.plan.manifest_digest or current_registry.digest != self.registry.digest:
            self.blockers.append("canonical manifest changed after planning; refusing to build stale intent")

    def _validate_source_prerequisites(self) -> None:
        for relative in self.plan.build.source_prerequisites:
            target = self.source_root / relative
            if not (target.exists() or target.is_symlink()):
                self.blockers.append(f"required build source is missing: {relative}")
            elif target.is_symlink():
                self.blockers.append(f"required build source must not be a symlink: {relative}")

    def _validate_private_paths(self) -> None:
        for label, path in (("build", self.build_dir), ("stage", self.stage_dir)):
            if not _lexically_within(path, self.installer_cache):
                self.blockers.append(f"{label} directory escapes installer cache: {path}")
            if path == self.installer_cache:
                self.blockers.append(f"{label} directory may not equal installer cache root")

    def _reset_private_directory(self, path: Path) -> None:
        try:
            if path.is_symlink():
                self.blockers.append(f"refusing symlinked installer-private directory: {path}")
                return
            if path.exists():
                shutil.rmtree(path)
            path.mkdir(parents=True, mode=0o700)
        except OSError as exc:
            self.blockers.append(f"cannot prepare installer-private directory {path}: {exc}")

    def _validate_cmake_cache(self) -> None:
        cache = _parse_cmake_cache(self.build_dir / "CMakeCache.txt")
        expected = {
            "CMAKE_HOME_DIRECTORY": str(self.source_root.resolve()),
            "CMAKE_GENERATOR": self.plan.build.generator,
            "CMAKE_BUILD_TYPE": self.plan.build.build_type,
            "CMAKE_INSTALL_PREFIX": self.plan.build.install_prefix,
            "CMAKE_INSTALL_BINDIR": "bin",
            "CMAKE_INSTALL_LIBDIR": "lib",
            "CMAKE_INSTALL_LIBEXECDIR": "libexec",
            "CMAKE_INSTALL_DATADIR": "share",
            "CMAKE_INSTALL_SYSCONFDIR": self.plan.layout.sysconf,
            "REALMHEART_EVENTD_AUTOSTART": "OFF",
            "REALMHEART_BUILD_HYPRLAND_PLUGIN": "ON",
            "REALMHEART_FX_BUILD_ID": self.plan.fx_plan.build_id,
            "REALMHEART_FX_HYPRLAND_COMMIT": self.plan.fx_plan.hyprland_commit or "",
            "REALMHEART_FX_HYPRLAND_ABI": self.plan.fx_plan.hyprland_abi_hash or "",
            "REALMHEART_FX_PLUGIN_PATH": str(Path(self.plan.layout.prefix) / "lib/realmheart/realmheart-fx.so"),
            "REALMHEART_ENABLE_NATIVE_WALLPAPER": "ON",
            "REALMHEART_ENABLE_SCREENSHOT": "ON",
            "BUILD_TESTING": "OFF",
        }
        for key, value in expected.items():
            observed = cache.get(key)
            if observed != value:
                self.blockers.append(f"CMake cache contract mismatch for {key}: expected {value!r}, observed {observed!r}")

    def _run_self_checks(self, env: dict[str, str]) -> bool:
        binary = self.build_dir / "realmheart"
        if not binary.is_file() or not os.access(binary, os.X_OK):
            self.blockers.append("built realmheart executable is missing or not executable")
            return False
        version = self._command("self-check:realmheart-version", (str(binary), "--version"), timeout=10.0, env=env)
        if not version.ok or self.plan.target_version not in (version.stdout + version.stderr):
            self.blockers.append("built realmheart --version does not match the planned release")
            return False
        for verification in self.plan.build.verification:
            verification_env = dict(env)
            verification_env.update(dict(verification.environment))
            test = self._command(
                f"self-check:{verification.id}",
                verification.argv,
                timeout=float(verification.timeout_seconds),
                env=verification_env,
            )
            if not test.ok:
                self.blockers.append(f"installer-safe build verification failed: {verification.id}")
                return False
        return True

    def _validate_staged_payload(self) -> None:
        expected: list[tuple[object, Path]] = []
        directory_roots: list[Path] = []
        for action in self.plan.artifact_actions:
            if not action.required:
                continue
            target = Path(action.target)
            if action.commit_class not in {ArtifactCommitClass.PRIVILEGED_COMMIT, ArtifactCommitClass.STAGED_PAYLOAD}:
                continue
            if not target.is_absolute():
                self.blockers.append(f"CMake-staged artifact target is not absolute: {action.artifact_id}")
                continue
            staged = self.stage_dir / target.relative_to("/")
            result = _inspect_staged_artifact(action, staged)
            self.artifact_results.append(result)
            expected.append((action, staged))
            if action.artifact_type == "directory":
                directory_roots.append(staged)
            if not result.ok:
                self.blockers.append(f"staged artifact invalid: {action.artifact_id}: {result.reason or 'validation failed'}")

        fx = next((item for item in self.artifact_results if item.artifact_id == "fx.plugin"), None)
        if self.plan.fx_plan.required and (fx is None or not fx.ok):
            self.blockers.append("required Realmheart FX plugin is absent/invalid in staged payload")

        auth = next((item for item in self.artifact_results if item.artifact_id == "auth.helper"), None)
        if auth and auth.exists:
            try:
                mode = stat.S_IMODE(Path(auth.staged_path).lstat().st_mode)
            except OSError as exc:
                self.blockers.append(f"cannot inspect staged auth helper mode: {exc}")
            else:
                if mode != 0o4755:
                    self.blockers.append(f"staged auth helper mode is {oct(mode)}, expected 0o4755")

        expected_exact = {path for _, path in expected if path not in directory_roots}
        allowed_auxiliary = {
            self.stage_dir / Path(target).relative_to("/")
            for target in self.plan.build.allowed_uncommitted_stage_paths
        }
        extras: list[str] = []
        accounted: list[str] = []
        if self.stage_dir.exists():
            for path in sorted(self.stage_dir.rglob("*")):
                if not path.is_symlink() and path.is_dir():
                    continue
                if path in expected_exact or any(_lexically_within(path, root) for root in directory_roots):
                    continue
                relative = str(path.relative_to(self.stage_dir))
                if path in allowed_auxiliary:
                    accounted.append(relative)
                else:
                    extras.append(relative)
        for path in sorted(allowed_auxiliary):
            if not (path.exists() or path.is_symlink()):
                if path.name == "realmheart-fx-load" and self.plan.fx_plan.required:
                    self.blockers.append("required configured Realmheart FX loader was not produced in staged payload")
                else:
                    self.warnings.append(f"declared staged-only auxiliary payload was not produced: {path.relative_to(self.stage_dir)}")
        self._validate_configured_fx_loader(allowed_auxiliary)
        if extras:
            self.warnings.append(
                "CMake staged unexpected payload outside the canonical live/auxiliary plan: "
                + ", ".join(extras)
            )
        self._accounted_uncommitted = tuple(accounted)
        self._unexpected = tuple(extras)

    def _validate_configured_fx_loader(self, allowed_auxiliary: set[Path]) -> None:
        loader = next((path for path in allowed_auxiliary if path.name == "realmheart-fx-load"), None)
        if loader is None or not loader.is_file() or loader.is_symlink():
            return
        try:
            text = loader.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            self.blockers.append(f"cannot inspect staged Realmheart FX loader identity: {exc}")
            return
        required = (
            self.plan.fx_plan.build_id,
            self.plan.fx_plan.hyprland_commit or "",
            self.plan.fx_plan.hyprland_abi_hash or "",
            str(Path(self.plan.layout.prefix) / "lib/realmheart/realmheart-fx.so"),
        )
        if "@REALMHEART_FX_" in text or any(value and value not in text for value in required):
            self.blockers.append("staged Realmheart FX loader is not bound to the approved build/Hyprland identity")

    def _live_target_fingerprints(self) -> dict[str, str]:
        targets: set[str] = set()
        for action in self.plan.config_actions:
            if action.will_mutate:
                targets.add(action.target)
        for action in self.plan.artifact_actions:
            if action.commit_class in {ArtifactCommitClass.PRIVILEGED_COMMIT, ArtifactCommitClass.STAGED_PAYLOAD}:
                targets.add(action.target)
        result: dict[str, str] = {}
        for target in sorted(targets):
            try:
                result[target] = fingerprint_path(Path(target))
            except OSError as exc:
                self.blockers.append(f"cannot establish pre-build live-target guard for {target}: {exc}")
        return result

    def _validate_live_targets(self, before: dict[str, str]) -> None:
        drifted: list[str] = []
        for target, expected in before.items():
            try:
                current = fingerprint_path(Path(target))
            except OSError as exc:
                self.blockers.append(f"cannot revalidate live target after build/staging: {target}: {exc}")
                drifted.append(target)
                continue
            if current != expected:
                drifted.append(target)
        self._live_drift = tuple(drifted)
        if drifted:
            self.blockers.append(
                "live Realmheart/config target(s) changed during build/staging: " + ", ".join(drifted)
            )

    def _eventd_unit_fingerprint(self) -> str | None:
        action = next((item for item in self.plan.config_actions if item.id == "config.generated.event.service"), None)
        if action is None:
            return None
        try:
            return fingerprint_path(Path(action.target))
        except OSError:
            return None

    def _eventd_runtime_signature(self) -> str | None:
        systemctl = _capability_executable(self.plan, "runtime.systemctl")
        if not systemctl:
            return None
        result = self.runner.run(
            (
                systemctl, "--user", "show", "realmheart-eventd.service",
                "--property=ActiveEnterTimestampMonotonic",
                "--property=ExecMainStartTimestampMonotonic",
                "--property=MainPID",
                "--property=UnitFileState",
            ),
            timeout=5.0,
        )
        return result.stdout.strip() if result.ok else None

    def _validate_eventd_side_effects(self, before_unit: str | None, before_runtime: str | None) -> None:
        after_unit = self._eventd_unit_fingerprint()
        if before_unit != after_unit:
            self.blockers.append("realmheart-eventd user service changed during installer-controlled build/staging")
        after_runtime = self._eventd_runtime_signature()
        if before_runtime is not None and after_runtime is not None and before_runtime != after_runtime:
            self.warnings.append("realmheart-eventd runtime signature changed during build window; build-time autostart is disabled, so treat this as concurrent external session activity unless logs prove otherwise")

    def _command(self, label: str, argv: tuple[str, ...], *, timeout: float, env: dict[str, str]) -> object:
        result = self.runner.run(argv, timeout=timeout, env=env, cwd=self.source_root)
        self.commands.append(BuildCommandRecord(
            label=label,
            argv=tuple(str(item) for item in argv),
            returncode=result.returncode,
            ok=result.ok,
            timed_out=result.timed_out,
            stdout_tail=_tail(result.stdout),
            stderr_tail=_tail(result.stderr),
        ))
        return result

    def _provenance(self) -> BuildProvenance | None:
        cache = _parse_cmake_cache(self.build_dir / "CMakeCache.txt")
        if not cache:
            return None
        cmake_version = _first_line(self.runner.run((self.plan.build.cmake_executable, "--version"), timeout=5.0).stdout)
        ninja_version = _first_line(self.runner.run((self.plan.build.ninja_executable, "--version"), timeout=5.0).stdout)
        compiler = cache.get("CMAKE_CXX_COMPILER")
        compiler_version = None
        if compiler:
            compiler_version = _first_line(self.runner.run((compiler, "--version"), timeout=5.0).stdout)
        return BuildProvenance(
            realmheart_version=self.plan.target_version,
            source_revision=self.plan.source_revision,
            source_dirty=self.plan.source_dirty,
            manifest_digest=self.plan.manifest_digest,
            plan_digest=self.plan.plan_digest,
            cmake_version=cmake_version,
            ninja_version=ninja_version,
            cxx_compiler=compiler,
            cxx_compiler_version=compiler_version,
            cmake_generator=cache.get("CMAKE_GENERATOR"),
            cmake_build_type=cache.get("CMAKE_BUILD_TYPE"),
            cmake_install_prefix=cache.get("CMAKE_INSTALL_PREFIX"),
            cmake_install_sysconfdir=cache.get("CMAKE_INSTALL_SYSCONFDIR"),
            eventd_autostart=cache.get("REALMHEART_EVENTD_AUTOSTART"),
            hyprland_version=self.plan.fx_plan.hyprland_version,
            hyprland_commit=self.plan.fx_plan.hyprland_commit,
            hyprland_abi_hash=self.plan.fx_plan.hyprland_abi_hash,
            fx_build_id=self.plan.fx_plan.build_id,
        )

    def _report(
        self,
        configured: bool,
        built: bool,
        checks: bool,
        installed: bool,
        eventd_unit_before: str | None,
        eventd_runtime_before: str | None,
    ) -> BuildStageReport:
        eventd_unit_after = self._eventd_unit_fingerprint()
        eventd_runtime_after = self._eventd_runtime_signature()
        unit_unchanged = eventd_unit_before == eventd_unit_after
        runtime_unchanged = None
        if eventd_runtime_before is not None and eventd_runtime_after is not None:
            runtime_unchanged = eventd_runtime_before == eventd_runtime_after
        payload_bytes = _tree_regular_bytes(self.stage_dir)
        accounted_uncommitted = getattr(self, "_accounted_uncommitted", ())
        unexpected = getattr(self, "_unexpected", ())
        blockers = tuple(dict.fromkeys(self.blockers))
        warnings = tuple(dict.fromkeys(self.warnings))
        state = BuildStageState.FAILED if blockers else BuildStageState.PASS
        live_drift = tuple(getattr(self, "_live_drift", ()))
        return BuildStageReport(
            schema_version=REPORT_SCHEMA_VERSION,
            transaction_id=self.plan.transaction_id,
            state=state,
            build_dir=str(self.build_dir),
            stage_dir=str(self.stage_dir),
            configured=configured,
            required_targets_built=built,
            self_checks_passed=checks,
            staged_install_completed=installed,
            live_targets_unchanged=not live_drift,
            drifted_live_targets=live_drift,
            eventd_unit_unchanged=unit_unchanged,
            eventd_runtime_signature_unchanged=runtime_unchanged,
            build_units=tuple(self.unit_results),
            artifacts=tuple(self.artifact_results),
            commands=tuple(self.commands),
            provenance=self._provenance(),
            staged_payload_bytes=payload_bytes,
            accounted_uncommitted_stage_paths=tuple(accounted_uncommitted),
            unexpected_stage_paths=tuple(unexpected),
            warnings=warnings,
            blockers=blockers,
        )


def _capability_executable(plan: InstallationPlan, capability_id: str) -> str | None:
    for item in plan.environment.capabilities:
        if item.capability_id == capability_id and item.executable:
            return item.executable
    return None


def _inspect_staged_artifact(action, staged: Path) -> StagedArtifactResult:
    exists = staged.exists() or staged.is_symlink()
    if not exists:
        return StagedArtifactResult(action.artifact_id, action.target, str(staged), action.artifact_type, action.required, False, False, None, None, None, None, None, "missing")
    try:
        st = staged.lstat()
    except OSError as exc:
        return StagedArtifactResult(action.artifact_id, action.target, str(staged), action.artifact_type, action.required, True, False, None, None, None, None, None, str(exc))
    if stat.S_ISLNK(st.st_mode):
        return StagedArtifactResult(action.artifact_id, action.target, str(staged), action.artifact_type, action.required, True, False, None, oct(stat.S_IMODE(st.st_mode)), None, None, None, "symlink is not accepted for a canonical staged artifact")
    wants_dir = action.artifact_type == "directory"
    type_ok = stat.S_ISDIR(st.st_mode) if wants_dir else stat.S_ISREG(st.st_mode)
    nested_symlink = None
    if type_ok and wants_dir:
        try:
            nested_symlink = _first_symlink_in_tree(staged)
        except OSError as exc:
            return StagedArtifactResult(
                action.artifact_id, action.target, str(staged), action.artifact_type, action.required,
                True, False, None, oct(stat.S_IMODE(st.st_mode)), None, None, None,
                f"cannot inspect staged directory safely: {exc}",
            )
    if nested_symlink is not None:
        return StagedArtifactResult(
            action.artifact_id, action.target, str(staged), action.artifact_type, action.required,
            True, False, None, oct(stat.S_IMODE(st.st_mode)), None, None, None,
            f"staged directory contains symlink: {nested_symlink.relative_to(staged)}",
        )
    executable_ok = None
    if action.artifact_type == "executable":
        executable_ok = type_ok and bool(stat.S_IMODE(st.st_mode) & 0o111)
    sha = _sha256_file(staged) if type_ok and not wants_dir else None
    fingerprint = None
    if type_ok:
        try:
            fingerprint = fingerprint_path(staged)
        except OSError:
            fingerprint = None
    reason = None
    if not type_ok:
        reason = "wrong filesystem type"
    elif executable_ok is False:
        reason = "expected executable bit is absent"
    return StagedArtifactResult(
        action.artifact_id,
        action.target,
        str(staged),
        action.artifact_type,
        action.required,
        True,
        type_ok,
        executable_ok,
        oct(stat.S_IMODE(st.st_mode)),
        _tree_regular_bytes(staged) if wants_dir else st.st_size,
        sha,
        fingerprint,
        reason,
    )


def _first_symlink_in_tree(path: Path) -> Path | None:
    """Return the first nested symlink without following symlinked directories."""
    stack = [path]
    while stack:
        current = stack.pop()
        with os.scandir(current) as entries:
            for entry in entries:
                child = Path(entry.path)
                if entry.is_symlink():
                    return child
                if entry.is_dir(follow_symlinks=False):
                    stack.append(child)
    return None


def _parse_cmake_cache(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return result
    for line in lines:
        if not line or line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        left, value = line.split("=", 1)
        key = left.split(":", 1)[0]
        if key:
            result[key] = value
    return result


def _sha256_file(path: Path) -> str | None:
    try:
        h = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return None


def _tree_regular_bytes(path: Path) -> int:
    if not path.exists() or path.is_symlink():
        return 0
    try:
        if path.is_file():
            return path.stat().st_size
        return sum(item.stat().st_size for item in path.rglob("*") if item.is_file() and not item.is_symlink())
    except OSError:
        return 0


def _lexically_within(path: Path, parent: Path) -> bool:
    try:
        path.absolute().relative_to(parent.absolute())
        return True
    except ValueError:
        return False


def _tail(text: str) -> str:
    if len(text) <= _TAIL_LIMIT:
        return text
    return text[-_TAIL_LIMIT:]


def _first_line(text: str) -> str | None:
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return None
