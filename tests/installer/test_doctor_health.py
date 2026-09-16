from __future__ import annotations

import hashlib
import json
import errno
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

import realmheart_doctor.health as health
from . import _bootstrap
from realmheart_doctor.health import (
    CommandObservation,
    HealthCheckExecutor,
    HealthStatus,
    SocketEndpoint,
    SocketObservation,
    _relocate_descriptor,
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


def _active_processes_for_command(argv: tuple[str, ...]) -> set[int]:
    """Return non-zombie PIDs whose /proc argv exactly matches ``argv``."""

    matches: set[int] = set()
    try:
        entries = tuple(Path("/proc").iterdir())
    except OSError:
        return matches
    for entry in entries:
        if not entry.name.isdecimal():
            continue
        try:
            raw_argv = tuple(
                item.decode("utf-8", errors="surrogateescape")
                for item in (entry / "cmdline").read_bytes().split(b"\0")
                if item
            )
            state = (entry / "stat").read_text(encoding="ascii").split(" ")[2]
        except (OSError, UnicodeError, IndexError):
            continue
        if state != "Z" and raw_argv == argv:
            matches.add(int(entry.name))
    return matches


def _process_is_absent(pid: int) -> bool:
    try:
        (Path(f"/proc/{pid}/stat")).read_text(encoding="ascii")
    except FileNotFoundError:
        return True
    except (OSError, UnicodeError):
        return False
    return False


def _wait_for_process_exit(pid: int, timeout: float = 1.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _process_is_absent(pid):
            return True
        time.sleep(0.01)
    return _process_is_absent(pid)


def _caller_subreaper_state() -> bool:
    """Read the caller's Linux child-subreaper bit without changing it."""
    if os.name != "posix":
        raise unittest.SkipTest("child-subreaper state is Linux-specific")
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    value = ctypes.c_int()
    result = libc.prctl(37, ctypes.byref(value), 0, 0, 0)
    if result != 0:
        raise unittest.SkipTest("PR_GET_CHILD_SUBREAPER is unavailable")
    return bool(value.value)


def _build_session_escape_probe(path: Path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        raise unittest.SkipTest("a C compiler is required for the session-escape lifecycle test")
    source = path.with_suffix(".c")
    source.write_text(
        """
#define _GNU_SOURCE
#include <sys/prctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 1 || prctl(PR_SET_PDEATHSIG, 0) != 0 || setsid() < 0) {
        return 90;
    }
    pid_t child = fork();
    if (child < 0) {
        return 91;
    }
    if (child > 0) {
        _exit(0);
    }
    char marker_path[4096];
    if (snprintf(marker_path, sizeof(marker_path), "%s.pid", argv[0]) < 0) {
        return 92;
    }
    FILE *marker = fopen(marker_path, "w");
    if (marker == NULL) {
        return 93;
    }
    fprintf(marker, "%ld\\n", (long)getpid());
    fclose(marker);
    for (;;) {
        pause();
    }
}
""",
        encoding="ascii",
    )
    subprocess.run(
        [compiler, "-O0", "-Wall", "-Werror", str(source), "-o", str(path)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=10,
    )
    path.chmod(0o755)


def _build_launch_marker_probe(path: Path) -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        raise unittest.SkipTest("a C compiler is required for the launch-gate test")
    source = path.with_suffix(".c")
    source.write_text(
        """
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 1) {
        return 90;
    }
    char marker_path[4096];
    if (snprintf(marker_path, sizeof(marker_path), "%s.launched", argv[0]) < 0) {
        return 91;
    }
    int descriptor = open(marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return 92;
    }
    if (write(descriptor, "launched\\n", 9) != 9) {
        close(descriptor);
        return 93;
    }
    close(descriptor);
    sleep(2);
    return 0;
}
""",
        encoding="ascii",
    )
    subprocess.run(
        [compiler, "-O0", "-Wall", "-Werror", str(source), "-o", str(path)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=10,
    )
    path.chmod(0o755)


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

    def test_worker_reaper_never_uses_unbounded_join(self):
        class UnconfirmedProcess:
            def __init__(self) -> None:
                self.join_timeouts: list[object] = []
                self.killed = False

            def is_alive(self) -> bool:
                return True

            def terminate(self) -> None:
                return None

            def kill(self) -> None:
                self.killed = True

            def join(self, *, timeout=None) -> None:
                self.join_timeouts.append(timeout)
                if timeout is None:
                    raise AssertionError("worker reaper attempted an unbounded join")

        process = UnconfirmedProcess()
        reaped = health._reap_health_worker(process, 0.01)

        self.assertFalse(reaped)
        self.assertTrue(process.killed)
        self.assertTrue(process.join_timeouts)
        self.assertTrue(all(timeout is not None for timeout in process.join_timeouts))

    def test_worker_handshake_rejects_a_group_leader_without_a_private_session(self):
        process = type("Process", (), {"pid": 424242})()
        with patch("realmheart_doctor.health.os.setpgid") as setpgid, patch(
            "realmheart_doctor.health.os.getsid", return_value=424241
        ), patch("realmheart_doctor.health.os.getpgid", return_value=424242):
            self.assertFalse(health._claim_health_worker_group(process))

        setpgid.assert_not_called()

    def test_process_snapshot_fails_closed_on_stat_loss_and_scan_truncation(self):
        class Entries:
            def __init__(self, names):
                self.names = names

            def __enter__(self):
                return iter(type("Entry", (), {"name": name})() for name in self.names)

            def __exit__(self, exc_type, exc_value, traceback):
                return False

        record = health._HealthProcessRecord(1, 0, 1, 1, 11, "S")
        with patch.object(health.os, "scandir", return_value=Entries(["1"])), patch.object(
            health, "_read_health_process_record", return_value=None
        ):
            self.assertIsNone(health._snapshot_health_processes())

        with patch.object(health.os, "scandir", return_value=Entries(["1", "2"])), patch.object(
            health, "_read_health_process_record", return_value=record
        ), patch.object(health, "_HEALTH_PROC_SCAN_LIMIT", 1):
            self.assertIsNone(health._snapshot_health_processes())

        with patch.object(health.os, "scandir", return_value=Entries(["self", "thread-self"])):
            self.assertIsNone(health._snapshot_health_processes())

    def test_request_handoff_binds_identity_and_existing_descendants(self):
        request = health._HealthProcessRecord(100, 1, 10, 10, 11, "S")
        existing = health._HealthProcessRecord(200, 100, 20, 20, 22, "S")
        records = {100: request, 200: existing}
        with patch.object(health, "_health_child_subreaper_state", return_value=True), patch.object(
            health.os, "getpid", return_value=100
        ), patch.object(health, "_read_health_process_record", return_value=request), patch.object(
            health, "_snapshot_health_processes", return_value=records
        ):
            handoff = health._establish_health_request_handoff()

        self.assertIsNotNone(handoff)
        assert handoff is not None
        identity, baseline = handoff
        self.assertEqual(identity.pid, 100)
        self.assertEqual(identity.start_time, 11)
        self.assertTrue(identity.child_subreaper)
        self.assertEqual(baseline, {200: 22})

        with patch.object(health, "_health_child_subreaper_state", return_value=True), patch.object(
            health.os, "getpid", return_value=100
        ), patch.object(health, "_read_health_process_record", return_value=request), patch.object(
            health, "_snapshot_health_processes", return_value=None
        ):
            self.assertIsNone(health._establish_health_request_handoff())

    def test_incomplete_snapshot_still_signals_known_tracked_identities(self):
        identity = health._HealthWorkerIdentity(
            424200,
            424200,
            424200,
            start_time=120,
            verified=True,
        )
        tracked: dict[int, int | None] = {424243: 123}
        with patch.object(health, "_health_sample_private_processes", return_value=None), patch.object(
            health, "_health_signal_tracked_descendants", return_value=True
        ) as signal_tracked:
            result = health._health_cleanup_boundary(identity, tracked, 0.0)

        self.assertFalse(result)
        signal_tracked.assert_called_once()

    def test_request_boundary_enumerates_only_new_adopted_descendants(self):
        helper = health._health_sample_request_owned_processes
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        records = {
            100: health._HealthProcessRecord(100, 1, 10, 10, 1, "S"),
            200: health._HealthProcessRecord(200, 100, 200, 200, 2, "Z"),
            300: health._HealthProcessRecord(300, 100, 300, 300, 3, "S"),
            400: health._HealthProcessRecord(400, 100, 400, 400, 4, "S"),
        }
        tracked: dict[int, int | None] = {}
        with patch.object(health, "_snapshot_health_processes", return_value=records):
            sampled = helper(
                request,
                tracked,
                {200: 2},
                excluded_identities={200: 2},
            )

        self.assertIsNotNone(sampled)
        assert sampled is not None
        descendants, valid = sampled
        self.assertTrue(valid)
        self.assertEqual(set(descendants), {300, 400})
        self.assertEqual(tracked, {300: 3, 400: 4})

    def test_request_boundary_rejects_an_excluded_identity_reuse(self):
        helper = health._health_sample_request_owned_processes
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        records = {
            100: health._HealthProcessRecord(100, 1, 10, 10, 1, "S"),
            200: health._HealthProcessRecord(200, 100, 200, 200, 99, "S"),
        }
        tracked: dict[int, int | None] = {}
        with patch.object(health, "_snapshot_health_processes", return_value=records):
            sampled = helper(
                request,
                tracked,
                {},
                excluded_identities={200: 2},
            )

        self.assertIsNotNone(sampled)
        assert sampled is not None
        descendants, valid = sampled
        self.assertFalse(valid)
        self.assertEqual(descendants, {})
        self.assertEqual(tracked, {})

    def test_request_boundary_partitions_stale_identity_from_matching_sibling(self):
        helper = health._health_sample_request_owned_processes
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        records = {
            100: health._HealthProcessRecord(100, 1, 10, 10, 1, "S"),
            200: health._HealthProcessRecord(200, 100, 200, 200, 99, "S"),
            300: health._HealthProcessRecord(300, 100, 300, 300, 3, "S"),
        }
        tracked: dict[int, int | None] = {200: 2, 300: 3}

        with patch.object(health, "_snapshot_health_processes", return_value=records):
            sampled = helper(request, tracked, {})

        self.assertIsNotNone(sampled)
        assert sampled is not None
        descendants, valid = sampled
        self.assertFalse(valid)
        self.assertEqual(set(descendants), {300})
        self.assertEqual(tracked, {200: None, 300: 3})

    def test_request_boundary_surfaces_signal_failure(self):
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        tracked: dict[int, int | None] = {300: 3}
        record = health._HealthProcessRecord(300, 100, 30, 30, 3, "S")
        with patch.object(
            health,
            "_health_sample_request_owned_processes",
            return_value=({300: record}, True),
        ), patch.object(health, "_health_signal_tracked_descendants", return_value=False) as signal_tracked, patch.object(
            health, "_health_reap_tracked_children", return_value=True
        ):
            result = health._health_cleanup_request_boundary(request, {}, tracked, 0.0)

        self.assertFalse(result)
        signal_tracked.assert_called_once()

    def test_request_cleanup_signals_and_reaps_matching_sibling_after_stale_identity(self):
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        records = {
            100: health._HealthProcessRecord(100, 1, 10, 10, 1, "S"),
            200: health._HealthProcessRecord(200, 100, 200, 200, 99, "S"),
            300: health._HealthProcessRecord(300, 100, 301, 300, 3, "S"),
        }
        tracked: dict[int, int | None] = {200: 2, 300: 3}
        sampled_calls = 0
        signalled: list[int] = []
        waited: list[int] = []
        real_sample_request_owned = health._health_sample_request_owned_processes

        def sample_request_owned(*args, **kwargs):
            nonlocal sampled_calls
            sampled_calls += 1
            if sampled_calls == 1:
                return real_sample_request_owned(
                    *args,
                    records=records,
                    **kwargs,
                )
            return {}, True

        def wait_tracked(current, **kwargs):
            waited.extend(pid for pid, start_time in current.items() if start_time == 3)
            current.pop(300, None)
            return True

        with patch.object(health, "_health_sample_request_owned_processes", side_effect=sample_request_owned), patch.object(
            health, "_read_health_process_record", side_effect=lambda pid: records.get(pid)
        ), patch.object(
            health,
            "_health_signal_process_identity",
            side_effect=lambda pid, *args, **kwargs: signalled.append(pid) or True,
        ), patch.object(health, "_health_reap_tracked_children", side_effect=wait_tracked), patch.object(
            health, "_health_tracked_processes_absent", return_value=True
        ):
            result = health._health_cleanup_request_boundary(request, {}, tracked, 0.03)

        self.assertFalse(result)
        self.assertEqual(signalled, [300])
        self.assertEqual(waited, [300])
        self.assertEqual(tracked, {200: None})

    def test_final_request_proof_rejects_a_descendant_seen_after_supervisor_pass(self):
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        descendant = health._HealthProcessRecord(300, 100, 30, 30, 3, "S")
        tracked: dict[int, int | None] = {300: 3}
        snapshots = iter((({300: descendant}, True), ({}, True), ({}, True)))

        def reap_tracked(current, **kwargs):
            current.clear()
            return True

        with patch.object(
            health,
            "_health_sample_request_owned_processes",
            side_effect=lambda *args, **kwargs: next(snapshots),
        ), patch.object(health, "_health_signal_tracked_descendants", return_value=True) as signal_tracked, patch.object(
            health, "_health_reap_tracked_children", side_effect=reap_tracked
        ), patch.object(health, "_health_tracked_processes_absent", return_value=True):
            result = health._health_cleanup_request_boundary(
                request,
                {},
                tracked,
                0.05,
                require_empty=True,
            )

        self.assertFalse(result)
        signal_tracked.assert_called_once()

    def test_final_request_proof_accepts_only_two_complete_empty_snapshots(self):
        request = health._HealthWorkerIdentity(
            100,
            10,
            10,
            start_time=1,
            child_subreaper=True,
            verified=True,
        )
        sample_calls = 0

        def sample_empty(*args, **kwargs):
            nonlocal sample_calls
            sample_calls += 1
            return {}, True

        with patch.object(health, "_health_sample_request_owned_processes", side_effect=sample_empty), patch.object(
            health, "_health_reap_tracked_children", return_value=True
        ), patch.object(health, "_health_tracked_processes_absent", return_value=True):
            result = health._health_cleanup_request_boundary(
                request,
                {},
                {},
                0.05,
                require_empty=True,
            )

        self.assertTrue(result)
        self.assertEqual(sample_calls, 2)

    def test_confirmed_supervisor_report_requires_final_request_proof(self):
        class DeadSupervisor:
            pid = 424242

            def is_alive(self):
                return False

            def join(self, *, timeout=None):
                self.timeout = timeout

        class ConfirmedCleanupConnection:
            def poll(self, timeout):
                return True

            def recv(self):
                return {"confirmed": True, "tracked": {424300: 777}}

        worker_identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        request_identity = health._HealthWorkerIdentity(
            424100,
            11,
            11,
            start_time=456,
            child_subreaper=True,
            verified=True,
        )
        stale_tracked: dict[int, int | None] = {424300: None}
        with patch.object(health, "_health_cleanup_request_boundary", return_value=False) as request_cleanup:
            result = health._reap_health_worker(
                DeadSupervisor(),
                0.01,
                process_group_owned=True,
                worker_identity=worker_identity,
                tracked_descendants=stale_tracked,
                request_identity=request_identity,
                request_baseline={},
                shutdown_event=threading.Event(),
                cleanup_connection=ConfirmedCleanupConnection(),
            )

        self.assertFalse(result)
        self.assertEqual(stale_tracked, {424300: None})
        request_cleanup.assert_called_once()
        self.assertTrue(request_cleanup.call_args.kwargs["require_empty"])

    def test_missing_supervisor_report_requires_final_request_proof_before_fallback_pass(self):
        class DeadSupervisor:
            pid = 424242

            def is_alive(self):
                return False

            def join(self, *, timeout=None):
                self.timeout = timeout

        class MissingCleanupConnection:
            def poll(self, timeout):
                return False

        worker_identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        request_identity = health._HealthWorkerIdentity(
            424100,
            11,
            11,
            start_time=456,
            child_subreaper=True,
            verified=True,
        )

        def cleanup_request(**kwargs):
            # Model a late descendant that cleanup can contain, but strict
            # final proof must still reject for an original PASS payload.
            return kwargs.get("require_empty") is not True

        with patch.object(health, "_health_cleanup_request_boundary", side_effect=cleanup_request) as request_cleanup:
            result = health._reap_health_worker(
                DeadSupervisor(),
                0.01,
                process_group_owned=True,
                worker_identity=worker_identity,
                tracked_descendants={424300: 777},
                request_identity=request_identity,
                request_baseline={},
                shutdown_event=threading.Event(),
                cleanup_connection=MissingCleanupConnection(),
                require_final_request_proof=True,
            )

        self.assertFalse(result)
        request_cleanup.assert_called_once()
        self.assertTrue(request_cleanup.call_args.kwargs["require_empty"])

    def test_post_kill_missing_supervisor_report_requires_final_request_proof(self):
        class PostKillSupervisor:
            pid = 424242

            def __init__(self):
                self.alive = True
                self.join_calls = 0

            def is_alive(self):
                return self.alive

            def join(self, *, timeout=None):
                self.join_calls += 1
                if self.join_calls >= 2:
                    self.alive = False

        class MissingCleanupConnection:
            def poll(self, timeout):
                return False

        worker_identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        request_identity = health._HealthWorkerIdentity(
            424100,
            11,
            11,
            start_time=456,
            child_subreaper=True,
            verified=True,
        )

        def cleanup_request(**kwargs):
            return kwargs.get("require_empty") is not True

        with patch.object(health, "_signal_health_worker", return_value=True) as signal_worker, patch.object(
            health, "_health_cleanup_request_boundary", side_effect=cleanup_request
        ) as request_cleanup:
            result = health._reap_health_worker(
                PostKillSupervisor(),
                0.01,
                process_group_owned=True,
                worker_identity=worker_identity,
                tracked_descendants={424300: 777},
                request_identity=request_identity,
                request_baseline={},
                shutdown_event=threading.Event(),
                cleanup_connection=MissingCleanupConnection(),
                require_final_request_proof=True,
            )

        self.assertFalse(result)
        signal_worker.assert_called_once()
        request_cleanup.assert_called_once()
        self.assertTrue(request_cleanup.call_args.kwargs["require_empty"])

    def test_pass_payload_cannot_survive_a_missing_supervisor_cleanup_report(self):
        if os.name != "posix":
            self.skipTest("supervisor-loss process-boundary test is POSIX-specific")

        for mode in ("report_missing", "post_kill"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                registry, _, _ = _manifest(root)
                marker = root / f"descendant-{mode}.pid"

                def raw_pass_supervisor(
                    _executor,
                    _registry,
                    _spec,
                    *,
                    connection,
                    ready_connection,
                    launch_event,
                    deadline,
                    **kwargs,
                ):
                    if not health._establish_health_worker_group():
                        os._exit(90)
                    ready_connection.send(health._health_worker_ready_message())
                    if not launch_event.wait(max(0.0, deadline - time.monotonic())):
                        os._exit(91)
                    # Keep the deliberately orphaned test child alive after
                    # this fake supervisor exits or is group-killed.
                    if not health._health_prctl(health._HEALTH_PR_SET_PDEATHSIG, 0):
                        os._exit(92)
                    connection.send(health._payload(HealthStatus.PASS, "probe_ok"))
                    child_pid = os.fork()
                    if child_pid == 0:
                        health._health_prctl(health._HEALTH_PR_SET_PDEATHSIG, 0)
                        marker.write_text(str(os.getpid()), encoding="ascii")
                        for resource in (connection, ready_connection, kwargs.get("cleanup_connection")):
                            if resource is not None:
                                try:
                                    resource.close()
                                except (OSError, ValueError):
                                    pass
                        while True:
                            time.sleep(1.0)
                    marker_deadline = time.monotonic() + 1.0
                    while not marker.exists() and time.monotonic() < marker_deadline:
                        time.sleep(0.001)
                    if mode == "report_missing":
                        os._exit(0)
                    while True:
                        time.sleep(1.0)

                checks = dict(registry.health_checks)
                checks["check.runtime"] = replace(
                    checks["check.runtime"],
                    timeout_ms=500,
                    args={"args": []},
                )
                registry = replace(registry, health_checks=checks)

                try:
                    with patch.object(health, "_run_health_supervisor", raw_pass_supervisor):
                        result = HealthCheckExecutor(
                            max_seconds=1.5,
                            worker_cleanup_seconds=0.05,
                        ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

                    self.assertEqual(result.status, HealthStatus.UNKNOWN)
                    self.assertEqual(result.reason_code, "worker_cleanup_incomplete")
                    self.assertTrue(marker.exists(), "the fake supervisor did not create its descendant")
                    descendant_pid = int(marker.read_text(encoding="ascii"))
                    self.assertTrue(
                        _wait_for_process_exit(descendant_pid),
                        f"the {mode} fallback left a reparented descendant alive",
                    )
                finally:
                    if marker.exists():
                        try:
                            descendant_pid = int(marker.read_text(encoding="ascii"))
                        except (OSError, ValueError):
                            descendant_pid = None
                        if descendant_pid is not None and not _process_is_absent(descendant_pid):
                            try:
                                os.kill(descendant_pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass

    def test_public_default_process_operations_require_the_supervisor_boundary(self):
        operations = health.ReadOnlyHealthOperations()
        with patch("realmheart_doctor.health.subprocess.Popen") as popen:
            result = operations.run(
                (APPROVED_TRUE_EXECUTABLE,),
                timeout=0.1,
                max_output_bytes=64,
            )

        self.assertEqual(result.error_code, "worker_boundary_required")
        popen.assert_not_called()

    def test_supervisor_loss_runs_request_side_identity_bound_cleanup_fallback(self):
        class DeadSupervisor:
            pid = 424242

            def is_alive(self):
                return False

            def join(self, *, timeout=None):
                self.timeout = timeout

        class EmptyCleanupConnection:
            def poll(self, timeout):
                return False

        identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        shutdown = threading.Event()
        with patch.object(health, "_health_cleanup_boundary", return_value=True) as cleanup:
            result = health._reap_health_worker(
                DeadSupervisor(),
                0.01,
                process_group_owned=True,
                worker_identity=identity,
                tracked_descendants={},
                shutdown_event=shutdown,
                cleanup_connection=EmptyCleanupConnection(),
            )

        self.assertTrue(result)
        cleanup.assert_called_once()

    def test_supervisor_loss_fallback_uses_the_request_handoff_boundary(self):
        class DeadSupervisor:
            pid = 424242

            def is_alive(self):
                return False

            def join(self, *, timeout=None):
                self.timeout = timeout

        class EmptyCleanupConnection:
            def poll(self, timeout):
                return False

        identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        request_identity = health._HealthWorkerIdentity(
            424100,
            11,
            11,
            start_time=456,
            child_subreaper=True,
            verified=True,
        )
        with patch.object(health, "_health_cleanup_request_boundary", return_value=True) as request_cleanup, patch.object(
            health, "_health_cleanup_boundary", side_effect=AssertionError("dead-root cleanup is insufficient")
        ):
            result = health._reap_health_worker(
                DeadSupervisor(),
                0.01,
                process_group_owned=True,
                worker_identity=identity,
                tracked_descendants={},
                request_identity=request_identity,
                request_baseline={},
                shutdown_event=threading.Event(),
                cleanup_connection=EmptyCleanupConnection(),
            )

        self.assertTrue(result)
        request_cleanup.assert_called_once()

    def test_pidfd_unavailability_uses_only_a_verified_private_process_group(self):
        record = health._HealthProcessRecord(424242, 1, 424242, 424242, 123, "S")
        with patch.object(health, "_read_health_process_record", return_value=record), patch.object(
            health, "_health_pidfd_open", return_value=None
        ), patch.object(health.os, "killpg") as killpg, patch.object(
            health.os, "kill", side_effect=AssertionError("raw PID signal is unsafe")
        ):
            result = health._health_signal_process_identity(424242, 123, signal.SIGKILL)

        self.assertTrue(result)
        killpg.assert_called_once_with(424242, signal.SIGKILL)

    def test_pidfd_unavailable_reaps_an_orphaned_private_group_without_raw_pid_signal(self):
        record = health._HealthProcessRecord(424243, 1, 424242, 424242, 123, "S")
        boundary = health._HealthWorkerIdentity(
            424200,
            424200,
            424200,
            start_time=120,
            verified=True,
        )
        with patch.object(health, "_read_health_process_record", return_value=record), patch.object(
            health, "_health_pidfd_open", return_value=None
        ), patch.object(health.os, "killpg") as killpg, patch.object(
            health.os, "kill", side_effect=AssertionError("raw PID signal is unsafe")
        ):
            result = health._health_signal_tracked_descendants(
                {424243: 123},
                signal.SIGKILL,
                private_boundary=boundary,
            )

        self.assertTrue(result)
        killpg.assert_called_once_with(424242, signal.SIGKILL)

    def test_pidfd_unavailable_never_kills_the_current_worker_group(self):
        record = health._HealthProcessRecord(424243, 424200, 424200, 424200, 123, "S")
        boundary = health._HealthWorkerIdentity(
            424200,
            424200,
            424200,
            start_time=120,
            verified=True,
        )
        with patch.object(health, "_read_health_process_record", return_value=record), patch.object(
            health, "_health_pidfd_open", return_value=None
        ), patch.object(health.os, "killpg") as killpg:
            result = health._health_signal_tracked_descendants(
                {424243: 123},
                signal.SIGKILL,
                protected_boundary=boundary,
            )

        self.assertFalse(result)
        killpg.assert_not_called()

    def test_reap_notifications_require_the_matching_process_identity(self):
        helper = getattr(health, "_health_apply_reap_notifications", None)
        self.assertTrue(callable(helper))
        tracked = {424242: 22}
        reaped = set()

        helper(tracked, reaped, {"reaped": ((424242, 21),)})
        self.assertEqual(tracked, {424242: 22})
        self.assertEqual(reaped, set())

        helper(tracked, reaped, {"reaped": ((424242, 22),)})
        self.assertEqual(tracked, {})
        self.assertEqual(reaped, {(424242, 22)})

    def test_launch_gate_final_permit_is_rechecked_before_exec(self):
        helper = getattr(health, "_health_launch_gate_allowed", None)
        self.assertTrue(callable(helper))
        read_descriptor, write_descriptor = os.pipe()
        try:
            self.assertFalse(
                helper(
                    time.monotonic() + 1.0,
                    read_descriptor,
                    final_check=lambda: False,
                )
            )
        finally:
            for descriptor in (read_descriptor, write_descriptor):
                try:
                    os.close(descriptor)
                except OSError:
                    pass

    def test_worker_loss_with_an_escaped_descendant_is_cleaned_before_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            _build_session_escape_probe(executable_path)
            marker = executable_path.with_name(executable_path.name + ".pid")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                timeout_ms=500,
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)
            real_popen = health.subprocess.Popen
            original_register = health._health_register_probe

            def wait_for_escape(*args, **kwargs):
                process = real_popen(*args, **kwargs)
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline and not marker.exists():
                    time.sleep(0.005)
                return process

            def lose_worker(pid: int) -> bool:
                registered = original_register(pid)
                os.kill(os.getpid(), signal.SIGKILL)
                return registered

            with patch("realmheart_doctor.health.subprocess.Popen", side_effect=wait_for_escape), patch(
                "realmheart_doctor.health._health_register_probe", side_effect=lose_worker
            ):
                result = HealthCheckExecutor(
                    max_seconds=0.5,
                    worker_cleanup_seconds=0.05,
                ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertTrue(marker.exists(), "the probe did not create an escaped descendant")
            escaped_pid = int(marker.read_text(encoding="ascii"))
            try:
                self.assertTrue(
                    _wait_for_process_exit(escaped_pid),
                    "an escaped descendant survived cleanup after worker loss",
                )
            finally:
                if not _process_is_absent(escaped_pid):
                    try:
                        os.kill(escaped_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_supervisor_loss_before_first_sample_refuses_probe_without_pidfds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            _build_session_escape_probe(executable_path)
            marker = executable_path.with_name(executable_path.name + ".pid")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                timeout_ms=500,
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            request_pid = os.getpid()
            real_private_sample = health._health_sample_private_processes
            request_sample_calls = 0

            def kill_supervisor_before_sample(identity, tracked):
                if (
                    os.getpid() != request_pid
                    and os.getppid() == request_pid
                    and health._HEALTH_WORKER_GROUP_OWNED
                    and marker.exists()
                ):
                    os.kill(os.getpid(), signal.SIGKILL)
                return real_private_sample(identity, tracked)

            real_request_sample = health._health_sample_request_owned_processes

            def defer_request_sampling(*args, **kwargs):
                nonlocal request_sample_calls
                request_sample_calls += 1
                if request_sample_calls <= 10:
                    return None
                return real_request_sample(*args, **kwargs)

            with patch.object(health, "_health_sample_private_processes", side_effect=kill_supervisor_before_sample), patch.object(
                health, "_health_sample_request_owned_processes", side_effect=defer_request_sampling
            ), patch.object(health, "_health_pidfd_open", return_value=None):
                result = HealthCheckExecutor(
                    max_seconds=0.5,
                    worker_cleanup_seconds=0.05,
                ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertGreaterEqual(request_sample_calls, 11)
            self.assertIn(result.reason_code, {"pidfd_unavailable", "launch_authority_unavailable"})
            self.assertFalse(marker.exists(), "the probe launched without pidfd authority")

    def test_identity_failure_never_falls_back_to_an_unbound_worker_signal(self):
        process = type("Process", (), {"pid": 424242})()
        identity = health._HealthWorkerIdentity(
            424242,
            424242,
            424242,
            start_time=123,
            verified=True,
        )
        with patch("realmheart_doctor.health._health_observe_worker_identity", return_value=None), patch(
            "realmheart_doctor.health._signal_health_worker_direct"
        ) as direct_signal:
            result = health._signal_health_worker(
                process,
                signal.SIGKILL,
                process_group_owned=True,
                worker_identity=identity,
            )

        self.assertFalse(result)
        direct_signal.assert_not_called()

    def test_pid_identity_signal_fails_closed_when_pidfds_are_unavailable(self):
        with patch.object(health.os, "pidfd_open", None, create=True), patch.object(
            health.os, "kill", side_effect=AssertionError("raw PID signal is unsafe")
        ):
            self.assertFalse(
                health._health_signal_tracked_descendants(
                    {424242: 123},
                    signal.SIGKILL,
                )
            )

    def test_reaping_requires_a_successful_waitpid_completion(self):
        record = health._HealthProcessRecord(424242, 1, 1, 1, 123, "Z")
        with patch.object(health, "_read_health_process_record", return_value=record), patch(
            "realmheart_doctor.health.os.waitpid", return_value=(0, 0)
        ):
            self.assertFalse(health._health_reap_tracked_children({424242: 123}))
        with patch.object(health, "_read_health_process_record", return_value=record), patch(
            "realmheart_doctor.health.os.waitpid", return_value=(424242, 0)
        ):
            self.assertTrue(health._health_reap_tracked_children({424242: 123}))
        for error in (ChildProcessError(), OSError(errno.ECHILD, "not a child"), OSError(errno.ESRCH, "gone")):
            with self.subTest(error=type(error).__name__), patch.object(
                health, "_read_health_process_record", return_value=record
            ), patch("realmheart_doctor.health.os.waitpid", side_effect=error):
                self.assertFalse(health._health_reap_tracked_children({424242: 123}))

    def test_reaping_refuses_a_reused_pid_before_waitpid(self):
        current = health._HealthProcessRecord(424242, 1, 1, 1, 999, "Z")
        tracked: dict[int, int | None] = {424242: 123}
        failed: set[int] = set()
        with patch.object(health, "_read_health_process_record", return_value=current), patch.object(
            health.os, "waitpid"
        ) as waitpid:
            result = health._health_reap_tracked_children(
                tracked,
                owner_pid=1,
                failed=failed,
            )

        self.assertFalse(result)
        self.assertEqual(tracked, {424242: 123})
        self.assertEqual(failed, {424242})
        waitpid.assert_not_called()

    def test_zombie_is_not_absent_without_reaping_proof(self):
        zombie = health._HealthProcessRecord(424242, 1, 1, 1, 123, "Z")
        with patch("realmheart_doctor.health._read_health_process_record", return_value=zombie):
            self.assertFalse(health._health_tracked_processes_absent({424242: 123}))

    def test_default_execution_does_not_change_caller_subreaper_state(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            before = _caller_subreaper_state()
            result = HealthCheckExecutor(max_seconds=1).execute(
                registry, check_ids=("check.runtime",)
            ).result_for("check.runtime")
            after = _caller_subreaper_state()

        self.assertEqual(result.status, HealthStatus.PASS)
        self.assertEqual(after, before)

    def test_subreaper_restore_preserves_an_already_enabled_caller_state(self):
        executor = HealthCheckExecutor()
        successful_payload = health._payload(HealthStatus.PASS, "observed")
        with patch.object(health, "_health_child_subreaper_state", side_effect=(True, True)), patch.object(
            health, "_set_health_child_subreaper"
        ) as set_subreaper, patch.object(
            executor,
            "_run_with_process_body",
            return_value=(successful_payload, 1.0),
        ):
            payload, _duration = executor._run_with_process(
                None,
                None,
                timeout_seconds=0.1,
                deadline=time.monotonic() + 1.0,
            )

        self.assertEqual(payload.status, HealthStatus.PASS)
        set_subreaper.assert_not_called()

    def test_default_execution_preserves_an_enabled_caller_subreaper_state(self):
        if os.name != "posix":
            self.skipTest("child-subreaper state is Linux-specific")
        before = _caller_subreaper_state()
        if not health._set_health_child_subreaper(True):
            self.skipTest("PR_SET_CHILD_SUBREAPER is unavailable")
        try:
            with tempfile.TemporaryDirectory() as temp:
                registry, _, _ = _manifest(Path(temp))
                result = HealthCheckExecutor(max_seconds=1).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")
                after = _caller_subreaper_state()
        finally:
            restored = health._set_health_child_subreaper(before)

        self.assertTrue(restored)
        self.assertEqual(result.status, HealthStatus.PASS)
        self.assertTrue(after)

    def test_subreaper_restore_returns_the_original_disabled_state(self):
        executor = HealthCheckExecutor()
        successful_payload = health._payload(HealthStatus.PASS, "observed")
        with patch.object(health, "_health_child_subreaper_state", side_effect=(False, False)), patch.object(
            health, "_set_health_child_subreaper", side_effect=(True, True)
        ) as set_subreaper, patch.object(
            executor,
            "_run_with_process_body",
            return_value=(successful_payload, 1.0),
        ):
            payload, _duration = executor._run_with_process(
                None,
                None,
                timeout_seconds=0.1,
                deadline=time.monotonic() + 1.0,
            )

        self.assertEqual(payload.status, HealthStatus.PASS)
        self.assertEqual(set_subreaper.call_args_list[0].args, (True,))
        self.assertEqual(set_subreaper.call_args_list[1].args, (False,))

    def test_subreaper_setter_failure_is_cleanup_incomplete_unknown(self):
        executor = HealthCheckExecutor()
        with patch.object(health, "_health_child_subreaper_state", side_effect=(False, False)), patch.object(
            health, "_set_health_child_subreaper", side_effect=(False, False)
        ) as set_subreaper, patch.object(executor, "_run_with_process_body") as body:
            payload, _duration = executor._run_with_process(
                None,
                None,
                timeout_seconds=0.1,
                deadline=time.monotonic() + 1.0,
            )

        self.assertEqual(payload.status, HealthStatus.UNKNOWN)
        self.assertEqual(payload.reason_code, "worker_cleanup_incomplete")
        body.assert_not_called()
        self.assertEqual(set_subreaper.call_count, 2)

    def test_subreaper_restore_failure_and_readback_mismatch_are_unknown(self):
        for states, setter_results in (
            ((False, False), (True, False)),
            ((False, True), (True, True)),
        ):
            with self.subTest(states=states, setter_results=setter_results):
                executor = HealthCheckExecutor()
                successful_payload = health._payload(HealthStatus.PASS, "observed")
                with patch.object(
                    health, "_health_child_subreaper_state", side_effect=states
                ), patch.object(
                    health, "_set_health_child_subreaper", side_effect=setter_results
                ), patch.object(
                    executor,
                    "_run_with_process_body",
                    return_value=(successful_payload, 1.0),
                ):
                    payload, _duration = executor._run_with_process(
                        None,
                        None,
                        timeout_seconds=0.1,
                        deadline=time.monotonic() + 1.0,
                    )

                self.assertEqual(payload.status, HealthStatus.UNKNOWN)
                self.assertEqual(payload.reason_code, "worker_cleanup_incomplete")

    def test_session_escape_descendant_is_reaped_before_timeout_returns(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            _build_session_escape_probe(executable_path)
            marker = executable_path.with_name(executable_path.name + ".pid")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                timeout_ms=120,
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            result = HealthCheckExecutor(
                max_seconds=0.5,
                worker_cleanup_seconds=0.05,
            ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"timeout", "budget_exhausted"})
            self.assertTrue(marker.exists(), "the probe did not create an escaped descendant")
            escaped_pid = int(marker.read_text(encoding="ascii"))
            try:
                self.assertTrue(
                    _wait_for_process_exit(escaped_pid),
                    "a setsid/double-fork descendant survived the bounded Doctor cleanup",
                )
            finally:
                if not _process_is_absent(escaped_pid):
                    try:
                        os.kill(escaped_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_session_escape_descendant_is_not_launched_without_pidfds(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            _build_session_escape_probe(executable_path)
            marker = executable_path.with_name(executable_path.name + ".pid")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                timeout_ms=120,
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            with patch("realmheart_doctor.health._health_pidfd_open", return_value=None):
                result = HealthCheckExecutor(
                    max_seconds=0.5,
                    worker_cleanup_seconds=0.05,
                ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"pidfd_unavailable", "launch_authority_unavailable"})
            self.assertFalse(marker.exists(), "the probe launched without pidfd authority")

    def test_group_signal_failure_makes_successful_probe_cleanup_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                timeout_ms=5000,
                args={"argv": [APPROVED_SLEEP_EXECUTABLE, "37.125"]},
            )
            registry = replace(registry, health_checks=checks)
            with patch("realmheart_doctor.health._health_pidfd_send_signal", return_value=False):
                result = HealthCheckExecutor(
                    max_seconds=1.0,
                    worker_cleanup_seconds=0.02,
                ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"worker_cleanup_incomplete", "budget_exhausted", "pidfd_unavailable"})

    def test_worker_reaper_reports_cleanup_api_errors_with_bounded_waits(self):
        class CleanupErrorProcess:
            pid = 424242

            def __init__(self) -> None:
                self.join_timeouts: list[object] = []

            def is_alive(self) -> bool:
                return True

            def terminate(self) -> None:
                raise RuntimeError("terminate failed")

            def kill(self) -> None:
                raise RuntimeError("kill failed")

            def join(self, *, timeout=None) -> None:
                self.join_timeouts.append(timeout)
                if timeout is None:
                    raise AssertionError("worker reaper attempted an unbounded join")
                raise RuntimeError("join failed")

        process = CleanupErrorProcess()
        started = time.monotonic()
        reaped = health._reap_health_worker(process, 0.01)
        elapsed = time.monotonic() - started

        self.assertFalse(reaped)
        self.assertLess(elapsed, 1.0)
        self.assertTrue(process.join_timeouts)
        self.assertTrue(all(timeout is not None for timeout in process.join_timeouts))

    def test_timeout_kills_probe_descendant_started_during_blocked_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, _ = _manifest(root)
            probe_argv = (APPROVED_SLEEP_EXECUTABLE, "37.125")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                timeout_ms=5000,
                args={"argv": list(probe_argv)},
            )
            registry = replace(registry, health_checks=checks)
            marker = root / "probe-pid"
            baseline = _active_processes_for_command(probe_argv)
            real_popen = health.subprocess.Popen

            def launch_then_block(*args, **kwargs):
                process = real_popen(*args, **kwargs)
                marker.write_text(str(process.pid), encoding="ascii")
                time.sleep(2.0)
                return process

            try:
                with patch("realmheart_doctor.health.subprocess.Popen", side_effect=launch_then_block):
                    started = time.monotonic()
                    result = HealthCheckExecutor(
                        # Leave enough scheduling room for the inner worker
                        # to create the real child before the request budget
                        # cancels the deliberately blocked Popen seam.
                        max_seconds=0.20,
                        worker_cleanup_seconds=0.02,
                    ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")
                    elapsed = time.monotonic() - started

                self.assertLess(elapsed, 1.0)
                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertIn(
                    result.reason_code,
                    {"timeout", "budget_exhausted", "worker_cleanup_incomplete"},
                )
                self.assertTrue(marker.exists(), "the probe launch race did not execute")
                launched_pid = int(marker.read_text(encoding="ascii"))

                active: set[int] = set()
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    active = _active_processes_for_command(probe_argv) - baseline
                    if not active:
                        break
                    time.sleep(0.01)
                self.assertNotIn(launched_pid, active)
                self.assertEqual(active, set())
            finally:
                for pid in _active_processes_for_command(probe_argv) - baseline:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_lost_readiness_still_reaps_worker_process_group(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, _ = _manifest(root)
            probe_argv = (APPROVED_SLEEP_EXECUTABLE, "37.125")
            marker = root / "probe-pid"
            baseline = _active_processes_for_command(probe_argv)

            def worker_without_readiness(*args, **kwargs):
                if not health._establish_health_worker_group():
                    return
                process = health.subprocess.Popen(
                    probe_argv,
                    stdin=health.subprocess.DEVNULL,
                    stdout=health.subprocess.DEVNULL,
                    stderr=health.subprocess.DEVNULL,
                    shell=False,
                    close_fds=True,
                    start_new_session=False,
                    env=dict(health._SANITIZED_ENVIRONMENT),
                )
                marker.write_text(str(process.pid), encoding="ascii")
                while True:
                    time.sleep(1.0)

            try:
                with patch("realmheart_doctor.health._run_health_check_in_process", worker_without_readiness):
                    started = time.monotonic()
                    result = HealthCheckExecutor(
                        max_seconds=0.04,
                        worker_cleanup_seconds=0.02,
                    ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")
                    elapsed = time.monotonic() - started

                self.assertLess(elapsed, 1.0)
                self.assertEqual(result.status, HealthStatus.UNKNOWN)
                self.assertTrue(marker.exists(), "the lost-readiness worker did not launch its descendant")
                launched_pid = int(marker.read_text(encoding="ascii"))

                active: set[int] = set()
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline:
                    active = _active_processes_for_command(probe_argv) - baseline
                    if not active:
                        break
                    time.sleep(0.01)
                self.assertNotIn(launched_pid, active)
                self.assertEqual(active, set())
            finally:
                for pid in _active_processes_for_command(probe_argv) - baseline:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_abnormal_worker_exit_returns_structured_unknown(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))

            def crash_worker(*args, **kwargs):
                os._exit(23)

            with patch("realmheart_doctor.health._run_health_check_in_process", crash_worker):
                started = time.monotonic()
                result = HealthCheckExecutor(
                    max_seconds=0.2,
                    worker_cleanup_seconds=0.01,
                ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")
                elapsed = time.monotonic() - started

            self.assertLess(elapsed, 1.0)
            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"operation_error", "budget_exhausted"})

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

    def test_descriptor_snapshot_rejects_same_inode_mutation_during_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            executable_path.write_bytes(Path(APPROVED_TRUE_EXECUTABLE).read_bytes())
            executable_path.chmod(0o755)
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            original_pread = health.os.pread
            mutated = False

            def mutate_after_snapshot_read(descriptor, size, offset):
                nonlocal mutated
                chunk = original_pread(descriptor, size, offset)
                if size > health.EXECUTABLE_HEADER_BYTES and offset == 0 and not mutated:
                    mutated = True
                    with executable_path.open("r+b") as stream:
                        stream.write(b"NOPE")
                        stream.flush()
                return chunk

            with patch("realmheart_doctor.health.os.pread", side_effect=mutate_after_snapshot_read), patch(
                "realmheart_doctor.health._run_bounded_process"
            ) as run:
                result = HealthCheckExecutor(max_seconds=2).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertEqual(result.reason_code, "artifact_snapshot_unstable")
            run.assert_not_called()

    def test_blocked_snapshot_cannot_launch_after_timeout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            executable_path.write_bytes(Path(APPROVED_TRUE_EXECUTABLE).read_bytes())
            executable_path.chmod(0o755)
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            release = threading.Event()
            launch_marker = root / "launch-marker"
            original_pread = health.os.pread

            def blocked_pread(descriptor, size, offset):
                chunk = original_pread(descriptor, size, offset)
                if size > health.EXECUTABLE_HEADER_BYTES:
                    release.wait()
                return chunk

            def record_launch(argv, **kwargs):
                launch_marker.write_text("launched", encoding="ascii")
                return CommandObservation(tuple(argv), 0)

            baseline_threads = {
                thread.ident
                for thread in threading.enumerate()
                if thread.name == "realmheart-doctor-probe"
            }
            with patch("realmheart_doctor.health.os.pread", side_effect=blocked_pread), patch(
                "realmheart_doctor.health._run_bounded_process", side_effect=record_launch
            ):
                try:
                    result = HealthCheckExecutor(
                        max_seconds=0.03,
                        worker_cleanup_seconds=0.01,
                    ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")
                    self.assertEqual(result.status, HealthStatus.UNKNOWN)
                    self.assertIn(
                        result.reason_code,
                        {"timeout", "budget_exhausted", "worker_cleanup_incomplete"},
                    )

                    release.set()
                    deadline = time.monotonic() + 1.0
                    while time.monotonic() < deadline and not launch_marker.exists():
                        time.sleep(0.005)
                    self.assertFalse(launch_marker.exists())
                finally:
                    release.set()
                    deadline = time.monotonic() + 1.0
                    while time.monotonic() < deadline:
                        active = {
                            thread.ident
                            for thread in threading.enumerate()
                            if thread.name == "realmheart-doctor-probe"
                        }
                        if active <= baseline_threads:
                            break
                        time.sleep(0.005)

    def test_launch_gate_rejects_a_child_that_reaches_exec_after_deadline(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            _build_launch_marker_probe(executable_path)
            marker = executable_path.with_name(executable_path.name + ".launched")
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                timeout_ms=50,
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)
            original_preexec = health._health_probe_preexec

            def delayed_preexec() -> None:
                time.sleep(0.12)
                original_preexec()

            with patch("realmheart_doctor.health._health_probe_preexec", side_effect=delayed_preexec):
                result = HealthCheckExecutor(max_seconds=1).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"timeout", "budget_exhausted"})
            self.assertFalse(marker.exists(), "the authorized ELF executed after its deadline")

    def test_pidfd_launch_authority_is_preflighted_before_probe_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            registry, _, _ = _manifest(Path(temp))

            with patch.object(health, "_health_pidfd_open", return_value=None), patch.object(
                health.subprocess, "Popen"
            ) as popen:
                result = HealthCheckExecutor(max_seconds=1).execute(
                    registry, check_ids=("check.runtime",)
                ).result_for("check.runtime")

            self.assertEqual(result.status, HealthStatus.UNKNOWN)
            self.assertIn(result.reason_code, {"pidfd_unavailable", "launch_authority_unavailable"})
            popen.assert_not_called()

    def test_repeated_snapshot_timeouts_do_not_accumulate_workers_or_descriptors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry, _, executable_path = _manifest(root)
            executable_path.write_bytes(Path(APPROVED_TRUE_EXECUTABLE).read_bytes())
            executable_path.chmod(0o755)
            checks = dict(registry.health_checks)
            checks["check.runtime"] = replace(
                checks["check.runtime"],
                artifact_id="demo.exec",
                args={"args": []},
            )
            registry = replace(registry, health_checks=checks)

            original_pread = health.os.pread
            current_block: list[tuple[threading.Event, Path] | None] = [None]
            gates: list[threading.Event] = []

            def blocked_pread(descriptor, size, offset):
                chunk = original_pread(descriptor, size, offset)
                if size > health.EXECUTABLE_HEADER_BYTES:
                    block = current_block[0]
                    if block is None:
                        raise AssertionError("snapshot read was not assigned a cancellation gate")
                    gate, entered = block
                    entered.write_text("entered", encoding="ascii")
                    gate.wait()
                return chunk

            def descriptor_count() -> int:
                return len(os.listdir("/proc/self/fd"))

            baseline_threads = {
                thread.ident
                for thread in threading.enumerate()
                if thread.name == "realmheart-doctor-probe"
            }
            baseline_descriptors = descriptor_count()
            with patch("realmheart_doctor.health.os.pread", side_effect=blocked_pread), patch(
                "realmheart_doctor.health._run_bounded_process",
                return_value=CommandObservation((str(executable_path),), 0),
            ):
                try:
                    for index in range(8):
                        gate = threading.Event()
                        entered = root / f"entered-{index}"
                        gates.append(gate)
                        current_block[0] = (gate, entered)
                        result = HealthCheckExecutor(
                            max_seconds=0.1,
                            worker_cleanup_seconds=0.01,
                        ).execute(registry, check_ids=("check.runtime",)).result_for("check.runtime")
                        self.assertEqual(result.status, HealthStatus.UNKNOWN)
                        self.assertIn(
                            result.reason_code,
                            {"timeout", "budget_exhausted", "worker_cleanup_incomplete"},
                        )
                        self.assertTrue(entered.exists())

                    active_threads = {
                        thread.ident
                        for thread in threading.enumerate()
                        if thread.name == "realmheart-doctor-probe"
                    }
                    self.assertLessEqual(len(active_threads - baseline_threads), 0)
                    self.assertLessEqual(descriptor_count() - baseline_descriptors, 1)
                finally:
                    for gate in gates:
                        gate.set()
                    deadline = time.monotonic() + 1.0
                    while time.monotonic() < deadline:
                        active = {
                            thread.ident
                            for thread in threading.enumerate()
                            if thread.name == "realmheart-doctor-probe"
                        }
                        if active <= baseline_threads:
                            break
                        time.sleep(0.005)

    def test_descriptor_relocation_closes_original_when_duplication_is_unavailable(self):
        with patch("realmheart_doctor.health.fcntl.F_DUPFD_CLOEXEC", None), patch(
            "realmheart_doctor.health.fcntl.F_DUPFD", None
        ), patch("realmheart_doctor.health.os.close") as close:
            result = _relocate_descriptor(2)

        self.assertEqual(result, (None, "descriptor_relocation_unavailable"))
        close.assert_called_once_with(2)

    def test_descriptor_relocation_closes_original_when_duplication_fails(self):
        with patch("realmheart_doctor.health.fcntl.fcntl", side_effect=OSError), patch(
            "realmheart_doctor.health.os.close"
        ) as close:
            result = _relocate_descriptor(2)

        self.assertEqual(result, (None, "descriptor_relocation_failed"))
        close.assert_called_once_with(2)

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
