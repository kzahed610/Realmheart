from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap
from realmheart_doctor.health import (
    CommandObservation,
    HealthCheckExecutor,
    HealthStatus,
    SocketEndpoint,
    SocketObservation,
)
from realmheart_maintenance.fingerprint import PathObservation
from realmheart_maintenance.manifest import load_manifest

PYTHON_EXECUTABLE = str(Path(sys.executable).resolve())
APPROVED_TRUE_EXECUTABLE = "/usr/bin/true"
APPROVED_PRINTF_EXECUTABLE = "/usr/bin/printf"
APPROVED_SLEEP_EXECUTABLE = "/usr/bin/sleep"


class FakeOperations:
    def __init__(self) -> None:
        self.paths: dict[Path, PathObservation] = {}
        self.files: dict[Path, bytes] = {}
        self.commands: dict[tuple[str, ...], CommandObservation] = {}
        self.sockets: dict[object, SocketObservation] = {}
        self.calls: list[tuple[str, object]] = []
        self.command_delay = 0.0

    def observe_path(self, path: Path, **kwargs) -> PathObservation:
        self.calls.append(("observe_path", Path(path)))
        return self.paths.get(Path(path), PathObservation(False, None, None))

    def read_regular_file(self, path: Path, **kwargs) -> bytes:
        self.calls.append(("read_regular_file", Path(path)))
        return self.files[Path(path)]

    def run(self, argv, **kwargs) -> CommandObservation:
        command = tuple(argv)
        self.calls.append(("run", command))
        if self.command_delay:
            time.sleep(self.command_delay)
        return self.commands.get(command, CommandObservation(command, 0))

    def run_descriptor(self, path: Path, argv, **kwargs) -> CommandObservation:
        command = tuple(argv)
        self.calls.append(("run_descriptor", (Path(path), command)))
        return self.commands.get(command, CommandObservation(command, 0))

    def socket_reachable(self, endpoint, **kwargs) -> SocketObservation:
        self.calls.append(("socket_reachable", endpoint))
        return self.sockets.get(endpoint, SocketObservation(False, error_code="connection_refused"))


def _manifest(root: Path):
    components = root / "components"
    components.mkdir()
    file_path = root / "demo.conf"
    executable_path = root / "demo-bin"
    expected_content = b'{"valid": true}\n'
    expected_hash = hashlib.sha256(expected_content).hexdigest()
    socket_path = root / "realmheart.sock"
    body = f'''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "demo"
name = "Demo"
component_version = "release"
category = "core"
stage = "foundation"

[[artifacts]]
id = "demo.file"
component_id = "demo"
path = "{file_path}"
type = "config"
required = true
ownership = "user"
managed = true

[[artifacts]]
id = "demo.exec"
component_id = "demo"
path = "{executable_path}"
type = "executable"
required = true
ownership = "user"
managed = true

[[health_checks]]
id = "check.exists"
component_id = "demo"
check = "artifact_exists"
artifact_id = "demo.file"
contexts = ["doctor_background"]

[[health_checks]]
id = "check.executable"
component_id = "demo"
check = "artifact_executable"
artifact_id = "demo.exec"
contexts = ["doctor_background"]

[[health_checks]]
id = "check.hash"
component_id = "demo"
check = "file_hash_matches"
artifact_id = "demo.file"
contexts = ["doctor_background"]
[health_checks.args]
sha256 = "{expected_hash}"

[[health_checks]]
id = "check.version"
component_id = "demo"
check = "version_probe"
contexts = ["doctor_background"]
[health_checks.args]
argv = ["{APPROVED_PRINTF_EXECUTABLE}", "Demo 1.2.3\\n"]
version_prefix = "demo"

[[health_checks]]
id = "check.runtime"
component_id = "demo"
check = "runtime_probe"
contexts = ["doctor_background"]
[health_checks.args]
argv = ["{APPROVED_TRUE_EXECUTABLE}"]

[[health_checks]]
id = "check.config"
component_id = "demo"
check = "config_parse"
artifact_id = "demo.file"
contexts = ["doctor_background"]
[health_checks.args]
format = "json"

[[health_checks]]
id = "check.socket"
component_id = "demo"
check = "socket_reachable"
contexts = ["doctor_background"]
[health_checks.args]
unix_path = "{socket_path}"

[[health_checks]]
id = "check.smoke"
component_id = "demo"
check = "process_start_smoke"
cost = "normal"
side_effects = "starts_component"
contexts = ["doctor_background"]
[health_checks.args]
argv = ["{APPROVED_TRUE_EXECUTABLE}"]
'''
    (components / "demo.toml").write_text(body, encoding="utf-8")
    return load_manifest(components), file_path, executable_path


class DoctorHealthExecutorTests(unittest.TestCase):
    def test_artifact_exists_and_executable_are_observed(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, file_path, executable_path = _manifest(Path(temp))
            ops = FakeOperations()
            ops.paths[file_path] = PathObservation(True, 0o644, "file")
            ops.paths[executable_path] = PathObservation(True, 0o755, "file")

            report = HealthCheckExecutor(ops).execute(
                registry,
                check_ids=("check.exists", "check.executable"),
            )

            self.assertEqual(report.result_for("check.exists").status, HealthStatus.PASS)
            self.assertEqual(report.result_for("check.executable").status, HealthStatus.PASS)
            self.assertEqual(len([call for call in ops.calls if call[0] == "observe_path"]), 2)

    def test_artifact_executable_requires_readable_non_special_mode(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, executable_path = _manifest(Path(temp))
            for mode, reason_code in (
                (0o111, "artifact_not_readable"),
                (0o4755, "special_mode_forbidden"),
                (0o2755, "special_mode_forbidden"),
            ):
                ops = FakeOperations()
                ops.paths[executable_path] = PathObservation(True, mode, "file")

                result = HealthCheckExecutor(ops).execute(
                    registry, check_ids=("check.executable",)
                ).result_for("check.executable")

                self.assertEqual(result.status, HealthStatus.FAIL)
                self.assertEqual(result.reason_code, reason_code)

    def test_missing_artifact_is_an_observed_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))

            result = HealthCheckExecutor(FakeOperations()).execute(registry).result_for("check.exists")

            self.assertEqual(result.status, HealthStatus.FAIL)
            self.assertEqual(result.reason_code, "artifact_missing")

    def test_hash_and_config_checks_are_supported(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, file_path, _ = _manifest(Path(temp))
            content = b'{"valid": true}\n'
            ops = FakeOperations()
            ops.paths[file_path] = PathObservation(True, 0o644, "file", sha256=hashlib.sha256(content).hexdigest())
            ops.files[file_path] = content

            report = HealthCheckExecutor(ops).execute(registry, check_ids=("check.hash", "check.config"))

            self.assertEqual(report.result_for("check.hash").status, HealthStatus.PASS)
            self.assertEqual(report.result_for("check.config").status, HealthStatus.PASS)

    def test_command_checks_use_structured_argv_and_support_version_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            ops.commands[(APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n")] = CommandObservation(
                (APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"), 0, stdout="Demo 1.2.3\n"
            )
            ops.commands[(APPROVED_TRUE_EXECUTABLE,)] = CommandObservation((APPROVED_TRUE_EXECUTABLE,), 0)

            report = HealthCheckExecutor(ops).execute(registry, check_ids=("check.version", "check.runtime"))

            self.assertEqual(report.result_for("check.version").status, HealthStatus.PASS)
            self.assertEqual(report.result_for("check.version").value, "1.2.3")
            self.assertEqual(report.result_for("check.runtime").status, HealthStatus.PASS)
            self.assertEqual([call[1] for call in ops.calls if call[0] == "run"], [
                (APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"),
                (APPROVED_TRUE_EXECUTABLE,),
            ])

    def test_socket_and_process_smoke_are_bounded_supported_types(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            endpoint = next(iter(registry.health_checks["check.socket"].args.values()))
            ops.sockets[SocketEndpoint("unix", path=endpoint)] = SocketObservation(True)
            ops.commands[(APPROVED_TRUE_EXECUTABLE,)] = CommandObservation((APPROVED_TRUE_EXECUTABLE,), 0)

            report = HealthCheckExecutor(ops).execute(
                registry,
                check_ids=("check.socket", "check.smoke"),
                max_cost="normal",
                allowed_side_effects=("none", "read_only", "starts_component"),
            )

            self.assertEqual(report.result_for("check.socket").status, HealthStatus.PASS)
            self.assertEqual(report.result_for("check.smoke").status, HealthStatus.PASS)

    def test_timeout_missing_executable_and_malformed_output_are_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            ops.commands[(APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n")] = CommandObservation(
                (APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"), None, timed_out=True, error_code="timeout"
            )
            timeout_report = HealthCheckExecutor(ops).execute(registry, check_ids=("check.version",))
            self.assertEqual(timeout_report.result_for("check.version").status, HealthStatus.UNKNOWN)
            self.assertEqual(timeout_report.result_for("check.version").reason_code, "timeout")

            ops.commands[(APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n")] = CommandObservation(
                (APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"), None, error_code="executable_missing"
            )
            missing_report = HealthCheckExecutor(ops).execute(registry, check_ids=("check.version",))
            self.assertEqual(missing_report.result_for("check.version").status, HealthStatus.UNKNOWN)
            self.assertEqual(missing_report.result_for("check.version").reason_code, "executable_missing")

            ops.commands[(APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n")] = CommandObservation(
                (APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"), 0, stdout="not a version\n"
            )
            malformed_report = HealthCheckExecutor(ops).execute(registry, check_ids=("check.version",))
            self.assertEqual(malformed_report.result_for("check.version").status, HealthStatus.UNKNOWN)
            self.assertEqual(malformed_report.result_for("check.version").reason_code, "malformed_output")

    def test_shell_text_is_rejected_without_calling_operations(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            check = registry.health_checks["check.runtime"]
            check.args["command"] = "demo --health; touch /tmp/should-not-exist"
            ops = FakeOperations()

            result = HealthCheckExecutor(ops).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "invalid_command")
            self.assertFalse(any(call[0] == "run" for call in ops.calls))

    def test_output_is_bounded_and_redacted(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            ops.commands[(APPROVED_TRUE_EXECUTABLE,)] = CommandObservation(
                (APPROVED_TRUE_EXECUTABLE,), 0,
                stdout="password=super-secret " + "x" * 200,
            )

            result = HealthCheckExecutor(ops, max_output_bytes=24).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "output_limit_exceeded")
            self.assertLessEqual(len((result.stdout + result.stderr).encode()), 24)
            self.assertNotIn("super-secret", result.stdout)

    def test_count_budget_and_run_scoped_cache_are_explicit(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            ops.paths[Path(registry.artifacts["demo.file"].path)] = PathObservation(True, 0o644, "file")

            report = HealthCheckExecutor(ops, max_checks=1).execute(
                registry, check_ids=("check.exists", "check.exists", "check.executable")
            )

            self.assertEqual(report.result_for("check.exists").status, HealthStatus.PASS)
            self.assertTrue(report.results[1].cached)
            self.assertEqual(report.results[2].status, HealthStatus.UNKNOWN)
            self.assertEqual(report.results[2].reason_code, "budget_exhausted")
            self.assertEqual(len([call for call in ops.calls if call[0] == "observe_path"]), 1)

            HealthCheckExecutor(ops).execute(registry, check_ids=("check.exists",))
            self.assertEqual(len([call for call in ops.calls if call[0] == "observe_path"]), 2)

    def test_policy_selection_is_canonical(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()

            report = HealthCheckExecutor(ops).execute(registry, context="doctor_background")

            self.assertNotIn("check.smoke", report.check_ids)
            self.assertIn("check.exists", report.check_ids)

    def test_unknown_check_id_and_unsupported_type_are_structured(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()

            report = HealthCheckExecutor(ops).execute(
                registry, check_ids=("missing.check",),
            )
            self.assertEqual(report.result_for("missing.check").status, HealthStatus.UNKNOWN)
            self.assertEqual(report.result_for("missing.check").reason_code, "missing_check_spec")
            self.assertEqual(ops.calls, [])

            unsupported = replace(
                registry.health_checks["check.exists"],
                id="future.check",
                check="future_probe",
            )
            unsupported_registry = replace(
                registry,
                health_checks={"future.check": unsupported},
            )
            unsupported_result = HealthCheckExecutor(ops).execute(
                unsupported_registry,
                check_ids=("future.check",),
            ).result_for("future.check")
            self.assertEqual(unsupported_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(unsupported_result.reason_code, "unsupported_check_type")
            self.assertEqual(ops.calls, [])

    def test_permission_errors_and_worker_budget_are_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))

            class PermissionOperations(FakeOperations):
                def observe_path(self, path: Path, **kwargs) -> PathObservation:
                    raise PermissionError("permission denied")

            permission_result = HealthCheckExecutor(PermissionOperations()).execute(
                registry,
                check_ids=("check.exists",),
            ).result_for("check.exists")
            self.assertEqual(permission_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(permission_result.reason_code, "permission_denied")

            delayed = FakeOperations()
            delayed.command_delay = 0.2
            report = HealthCheckExecutor(
                delayed,
                max_seconds=0.03,
                worker_cleanup_seconds=0.01,
            ).execute(registry, check_ids=("check.runtime",))
            result = report.result_for("check.runtime")
            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"timeout", "budget_exhausted"})
            self.assertTrue(report.budget_exhausted)

    def test_default_filesystem_operations_are_descriptor_bound_and_read_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, file_path, executable_path = _manifest(root)
            content = b'{"valid": true}\n'
            file_path.write_bytes(content)
            executable_path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            executable_path.chmod(0o755)

            report = HealthCheckExecutor().execute(
                registry,
                check_ids=("check.exists", "check.executable", "check.hash", "check.config"),
            )

            self.assertTrue(all(result.status is HealthStatus.PASS for result in report.results))

    def test_default_process_operation_enforces_output_and_timeout_bounds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, _ = _manifest(root)
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                timeout_ms=80,
                args={"argv": [APPROVED_SLEEP_EXECUTABLE, "2"]},
            )
            checks["check.version"] = replace(
                checks["check.version"],
                args={
                    "argv": [APPROVED_PRINTF_EXECUTABLE, "Demo 1.2.3\n"],
                    "version_prefix": "demo",
                },
            )
            registry = replace(registry, health_checks=checks)

            timeout_result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            self.assertEqual(timeout_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(timeout_result.reason_code, "timeout")

            output_registry = replace(
                registry,
                health_checks={
                    **checks,
                    "check.runtime": replace(
                        checks["check.runtime"],
                        timeout_ms=1000,
                        args={"argv": [APPROVED_PRINTF_EXECUTABLE, "x" * 1000]},
                    ),
                },
            )
            output_result = HealthCheckExecutor(max_output_bytes=32, max_seconds=2).execute(
                output_registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            self.assertEqual(output_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(output_result.reason_code, "output_limit_exceeded")
            self.assertTrue(output_result.output_truncated)

            version_result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")
            self.assertEqual(version_result.status, HealthStatus.PASS)
            self.assertEqual(version_result.value, "1.2.3")

    def test_artifact_backed_command_uses_canonical_path_and_tail_arguments(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, executable_path = _manifest(Path(temp))
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)
            ops = FakeOperations()
            canonical_argv = (str(executable_path), "--version")
            ops.commands[canonical_argv] = CommandObservation(canonical_argv, 0, stdout="Demo 1.2.3\n")

            result = HealthCheckExecutor(ops).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.PASS)
            self.assertEqual(
                [call[1] for call in ops.calls if call[0] == "run_descriptor"],
                [(executable_path, canonical_argv)],
            )

            checks["check.version"] = replace(
                checks["check.version"],
                args={"argv": ["/tmp/not-the-canonical-artifact", "--version"]},
            )
            rejected_registry = replace(registry, health_checks=checks)
            rejected_ops = FakeOperations()
            rejected = HealthCheckExecutor(rejected_ops).execute(
                rejected_registry, check_ids=("check.version",)
            ).result_for("check.version")
            self.assertEqual(rejected.status, HealthStatus.UNKNOWN)
            self.assertEqual(rejected.reason_code, "invalid_command")
            self.assertEqual(rejected_ops.calls, [])

    def test_shell_wrappers_and_indirect_launchers_are_rejected_unconditionally(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            check = registry.health_checks["check.runtime"]

            for argv in (
                ["/bin/sh"],
                ["bash", "-s"],
                ["env", "sh", "/tmp/probe.sh"],
                ["command", "sh", "/tmp/probe.sh"],
            ):
                check.args["argv"] = argv
                result = HealthCheckExecutor(ops).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")

                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertEqual(result.reason_code, "invalid_command")
                self.assertFalse(any(call[0] == "run" for call in ops.calls))

    def test_artifactless_command_requires_absolute_executable_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            check = registry.health_checks["check.runtime"]
            check.args["argv"] = ["demo", "--health"]

            result = HealthCheckExecutor(ops).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "invalid_command")
            self.assertFalse(any(call[0] == "run" for call in ops.calls))

    def test_artifact_backed_command_requires_declared_executable_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, executable_path = _manifest(Path(temp))
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.file",
                args={"args": ["--version"]},
            )

            for artifact_type in ("config", "service", "library", "file"):
                artifacts = dict(registry.artifacts)
                artifacts["demo.file"] = replace(
                    artifacts["demo.file"],
                    type=artifact_type,
                    path=str(executable_path),
                )
                candidate = replace(
                    registry,
                    artifacts=artifacts,
                    health_checks=checks,
                )
                ops = FakeOperations()
                result = HealthCheckExecutor(ops).execute(
                    candidate, check_ids=("check.version",)
                ).result_for("check.version")

                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertEqual(result.reason_code, "artifact_type_mismatch")
                self.assertFalse(any(call[0] in {"run", "run_descriptor"} for call in ops.calls))

    def test_artifact_backed_command_uses_descriptor_operation(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, executable_path = _manifest(Path(temp))
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)
            ops = FakeOperations()
            canonical_argv = (str(executable_path), "--version")
            ops.commands[canonical_argv] = CommandObservation(
                canonical_argv, 0, stdout="Demo 1.2.3\n"
            )

            result = HealthCheckExecutor(ops).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.PASS)
            self.assertEqual(
                [call[1] for call in ops.calls if call[0] == "run_descriptor"],
                [(executable_path, canonical_argv)],
            )
            self.assertFalse(any(call[0] == "run" for call in ops.calls))

    def test_default_descriptor_operation_executes_regular_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            artifacts = dict(registry.artifacts)
            artifacts["demo.exec"] = replace(
                artifacts["demo.exec"],
                path=APPROVED_PRINTF_EXECUTABLE,
            )
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={
                    "args": ["Demo 1.2.3\n"],
                    "version_prefix": "demo",
                },
            )
            registry = replace(
                registry,
                artifacts=artifacts,
                health_checks=checks,
            )

            result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.PASS)
            self.assertEqual(result.value, "1.2.3")

    def test_descriptor_operation_relocates_executable_away_from_stdio(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            artifacts = dict(registry.artifacts)
            artifacts["demo.exec"] = replace(
                artifacts["demo.exec"],
                path=APPROVED_TRUE_EXECUTABLE,
            )
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                args={"args": []},
            )
            registry = replace(registry, artifacts=artifacts, health_checks=checks)

            saved_fds: list[int | None] = []
            for fd in range(3):
                try:
                    saved_fds.append(os.dup(fd))
                except OSError:
                    saved_fds.append(None)
            try:
                for fd in range(3):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                result = HealthCheckExecutor(max_seconds=2).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")
            finally:
                for fd, saved_fd in enumerate(saved_fds):
                    if saved_fd is not None:
                        os.dup2(saved_fd, fd)
                        os.close(saved_fd)

            self.assertEqual(result.status, HealthStatus.PASS)

    def test_shell_shebang_artifact_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            marker = root / "shell-marker"
            executable_path.write_text(
                f"#!/bin/sh\nprintf touched > {marker}\nprintf 'Demo 1.2.3\\n'\n",
                encoding="utf-8",
            )
            executable_path.chmod(0o755)
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)

            result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "unsupported_executable")
            self.assertFalse(marker.exists())

    def test_env_shebang_is_rejected_without_inherited_path_lookup(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            marker = root / "env-marker"
            interpreter = bin_dir / "doctor-env-probe"
            interpreter.write_text(
                f"#!/bin/sh\nprintf touched > {marker}\nprintf 'Demo 1.2.3\\n'\n",
                encoding="utf-8",
            )
            interpreter.chmod(0o755)
            executable_path.write_text("#!/usr/bin/env doctor-env-probe\n", encoding="utf-8")
            executable_path.chmod(0o755)
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)

            with patch.dict("os.environ", {"PATH": str(bin_dir)}, clear=False):
                result = HealthCheckExecutor(max_seconds=2).execute(
                    registry, check_ids=("check.version",)
                ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "unsupported_executable")
            self.assertFalse(marker.exists())

    def test_interpreters_and_alternate_launchers_require_approved_identity_and_schema(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            check = registry.health_checks["check.runtime"]
            for argv in (
                [PYTHON_EXECUTABLE, "-c", "open('/tmp/doctor-marker', 'w').write('ran')"],
                ["/usr/bin/setsid", APPROVED_TRUE_EXECUTABLE],
                ["/usr/bin/systemd-run", "--wait", APPROVED_TRUE_EXECUTABLE],
                ["/usr/bin/su", "-c", APPROVED_TRUE_EXECUTABLE],
            ):
                check.args["argv"] = argv
                ops = FakeOperations()

                result = HealthCheckExecutor(ops).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")

                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertEqual(result.reason_code, "invalid_command")
                self.assertEqual(ops.calls, [])

    def test_setuid_and_setgid_executables_are_rejected_before_execution(self):
        for special_mode in (0o4755, 0o2755, 0o6755):
            with self.subTest(mode=oct(special_mode)), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, _, executable_path = _manifest(root)
                marker = root / "special-mode-marker"
                executable_path.write_text(
                    f"#!/bin/sh\nprintf touched > {marker}\nprintf 'Demo 1.2.3\\n'\n",
                    encoding="utf-8",
                )
                executable_path.chmod(special_mode)
                checks = dict(registry.health_checks)
                checks["check.version"] = replace(
                    checks["check.version"],
                    artifact_id="demo.exec",
                    args={"args": ["--version"]},
                )
                registry = replace(registry, health_checks=checks)

                result = HealthCheckExecutor(max_seconds=2).execute(
                    registry, check_ids=("check.version",)
                ).result_for("check.version")

                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertEqual(result.reason_code, "special_mode_forbidden")
                self.assertFalse(marker.exists())

    def test_approved_elf_artifact_remains_executable(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            artifacts = dict(registry.artifacts)
            artifacts["demo.exec"] = replace(
                artifacts["demo.exec"],
                path=APPROVED_TRUE_EXECUTABLE,
            )
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                args={"args": []},
            )
            registry = replace(registry, artifacts=artifacts, health_checks=checks)

            result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.PASS)

    def test_default_descriptor_operation_rejects_symlinked_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            target = root / "real-demo"
            target.write_text("#!/bin/sh\nprintf 'Demo 1.2.3\\n'\n", encoding="utf-8")
            target.chmod(0o755)
            executable_path.symlink_to(target)
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)

            result = HealthCheckExecutor(max_seconds=2).execute(
                registry, check_ids=("check.version",)
            ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "symlink_forbidden")

    def test_descriptor_execution_unavailability_fails_closed_without_path_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            checks = dict(registry.health_checks)
            checks["check.version"] = replace(
                checks["check.version"],
                artifact_id="demo.exec",
                args={"args": ["--version"]},
            )
            registry = replace(registry, health_checks=checks)

            with patch(
                "realmheart_doctor.health._descriptor_execution_available",
                return_value=False,
            ), patch("realmheart_doctor.health.subprocess.Popen") as popen:
                result = HealthCheckExecutor(max_seconds=2).execute(
                    registry, check_ids=("check.version",)
                ).result_for("check.version")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "descriptor_execution_unavailable")
            popen.assert_not_called()

    def test_nested_shell_and_privilege_wrappers_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            ops = FakeOperations()
            check = registry.health_checks["check.runtime"]

            check.args["argv"] = ["env", "sh", "-c", "touch /tmp/should-not-exist"]
            nested_shell = HealthCheckExecutor(ops).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            self.assertEqual(nested_shell.status, HealthStatus.UNKNOWN)
            self.assertEqual(nested_shell.reason_code, "invalid_command")
            self.assertEqual(ops.calls, [])

            check.args["argv"] = ["sudo", "true"]
            privilege = HealthCheckExecutor(ops).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            self.assertEqual(privilege.status, HealthStatus.UNKNOWN)
            self.assertEqual(privilege.reason_code, "invalid_command")
            self.assertEqual(ops.calls, [])

    def test_malformed_operation_observations_remain_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))

            class MalformedOperations(FakeOperations):
                def run(self, argv, **kwargs):
                    return {"returncode": "0", "stdout": "Runtime 1.2.3"}

            command_result = HealthCheckExecutor(MalformedOperations()).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            self.assertEqual(command_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(command_result.reason_code, "malformed_output")

            class MalformedSocketOperations(FakeOperations):
                def socket_reachable(self, endpoint, **kwargs):
                    return {"reachable": "yes"}

            socket_result = HealthCheckExecutor(MalformedSocketOperations()).execute(
                registry, check_ids=("check.socket",)
            ).result_for("check.socket")
            self.assertEqual(socket_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(socket_result.reason_code, "socket_unavailable")

            class MalformedPathOperations(FakeOperations):
                def observe_path(self, path: Path, **kwargs):
                    return None

            path_result = HealthCheckExecutor(MalformedPathOperations()).execute(
                registry, check_ids=("check.exists",)
            ).result_for("check.exists")
            self.assertEqual(path_result.status, HealthStatus.UNKNOWN)
            self.assertEqual(path_result.reason_code, "observation_unavailable")


if __name__ == "__main__":
    unittest.main()
