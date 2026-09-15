"""Bounded, read-only Doctor health-check execution.

The manifest remains the only source of check identity and policy.  This module
only translates canonical ``HealthCheckSpec`` records into bounded observations;
it never repairs, activates, or mutates Realmheart state.
"""
from __future__ import annotations

import configparser
import errno
import fcntl
import hashlib
import inspect
import json
import math
import multiprocessing
import os
import re
import selectors
import signal
import socket
import stat
import subprocess
import threading
import time
import tomllib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Protocol, cast, runtime_checkable

from realmheart_maintenance.fingerprint import (
    FingerprintLimitExceeded,
    FingerprintObservationError,
    MAX_FINGERPRINT_BYTES,
    PathObservation,
    observe_path,
    read_regular_file,
)
from realmheart_maintenance.forensics import select_health_checks
from realmheart_maintenance.manifest import (
    ManifestRegistry,
    ParsedVersion,
    VersionSpec,
    classify_version,
    resolve_canonical_artifact_path,
)


SUPPORTED_CHECK_TYPES = frozenset(
    {
        "artifact_exists",
        "artifact_executable",
        "file_hash_matches",
        "version_probe",
        "runtime_probe",
        "config_parse",
        "socket_reachable",
        "process_start_smoke",
    }
)

# These are deliberately finite hard ceilings.  The executor's public defaults
# are substantially lower, but a malformed or untrusted manifest cannot turn a
# single Doctor run into an unbounded process.
MAX_CHECKS = 4096
MAX_RUN_SECONDS = 300.0
MAX_CHECK_SECONDS = 300.0
MAX_OUTPUT_BYTES = 4 * 1024 * 1024
MAX_FILE_BYTES = MAX_FINGERPRINT_BYTES
MAX_SOCKET_SECONDS = 5.0
MAX_WORKER_CLEANUP_SECONDS = 0.25
MAX_ARGUMENTS = 128
MAX_ARGUMENT_BYTES = 64 * 1024
MAX_DETAIL_BYTES = 512
MAX_VERSION_TOKEN_BYTES = 256
MAX_SOCKET_PATH_BYTES = 107
MIN_PASSTHROUGH_FD = 3
EXECUTABLE_HEADER_BYTES = 4
EXECUTABLE_SNAPSHOT_CHUNK_BYTES = 1024 * 1024
MAX_EXECUTABLE_SNAPSHOT_BYTES = MAX_FILE_BYTES

_VERSION_TOKEN_RE = re.compile(
    r"(?<![A-Za-z0-9])v?(\d+\.\d+(?:\.\d+)?(?:[-+][0-9A-Za-z.-]+)?)(?![A-Za-z0-9])"
)
_SECRET_ASSIGNMENT_RE = re.compile(
    r"(?i)((?:[\"']?)(?:password|passwd|token|secret|api[_-]?key|authorization|credential|private[_-]?key)(?:[\"']?)\s*[:=]\s*)(\"[^\"]*\"|'[^']*'|[^\s,;}]+)"
)
_SECRET_FLAG_RE = re.compile(
    r"(?i)((?:--?)(?:password|passwd|token|secret|api[_-]?key|authorization|credential|private[_-]?key)(?:=|\s+))(\"[^\"]*\"|'[^']*'|[^\s,;}]+)"
)
_BEARER_RE = re.compile(r"(?i)(\bbearer\s+)([^\s,;]+)")
_APPROVED_VERSION_FLAGS = frozenset({"--version", "-V", "--help", "-h"})
_APPROVED_PROBE_FLAGS = frozenset(
    {"--version", "-V", "--help", "-h", "--health", "--smoke", "--status", "--check"}
)
_APPROVED_ARTIFACTLESS_EXECUTABLES = {
    # Artifact-less probes are intentionally limited to fixed, read-only
    # utility identities.  There is no PATH lookup or basename inference.
    "/usr/bin/false": "no_args",
    "/usr/bin/printf": "printf",
    "/usr/bin/sleep": "sleep",
    "/usr/bin/true": "no_args",
}
_SANITIZED_ENVIRONMENT = {
    "LANG": "C",
    "LC_ALL": "C",
    "LC_CTYPE": "C",
    "PATH": "/usr/bin:/bin",
    "TZ": "UTC",
}
_REASON_CODE_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SECRET_REPLACEMENT = "[REDACTED]"


@dataclass
class _CancellationToken:
    """Carry one absolute operation deadline and cooperative cancellation."""

    deadline: float
    event: Any = field(default_factory=threading.Event)
    clock: Callable[[], float] = time.monotonic

    def cancel(self) -> None:
        self.event.set()

    def is_cancelled(self) -> bool:
        return bool(self.event.is_set()) or self.clock() >= self.deadline

    def remaining(self) -> float:
        return max(0.0, self.deadline - self.clock())


class HealthStatus(str, Enum):
    """The four deliberately explicit Doctor observation outcomes."""

    PASS = "pass"
    FAIL = "fail"
    UNKNOWN = "unknown"
    NOT_APPLICABLE = "not_applicable"
    # A skipped check is represented by the same machine-readable state.  The
    # alias keeps callers from inventing a fifth semantic state.
    SKIPPED = "not_applicable"


HealthCheckStatus = HealthStatus
HealthCheckState = HealthStatus
HealthCheckOutcome = HealthStatus


@dataclass(frozen=True)
class CommandObservation:
    """Bounded result returned by the process-operation seam."""

    argv: tuple[str, ...]
    returncode: int | None = None
    stdout: str | bytes = ""
    stderr: str | bytes = ""
    timed_out: bool = False
    output_limited: bool = False
    error_code: str | None = None
    error_detail: str | None = None
    duration_ms: float = 0.0

    @property
    def ok(self) -> bool:
        return (
            type(self.returncode) is int
            and self.returncode == 0
            and not self.timed_out
            and not self.output_limited
            and self.error_code is None
        )


@dataclass(frozen=True)
class SocketEndpoint:
    """An endpoint that has already passed Doctor's local-only validation."""

    kind: str
    path: str | None = None
    host: str | None = None
    port: int | None = None

    def cache_key(self) -> tuple[object, ...]:
        return (self.kind, self.path, self.host, self.port)


@dataclass(frozen=True)
class SocketObservation:
    """Bounded result returned by the socket-operation seam."""

    reachable: bool
    error_code: str | None = None
    error_detail: str | None = None
    duration_ms: float = 0.0


def _operation_cancelled(
    deadline: float | None,
    cancellation: _CancellationToken | None,
) -> bool:
    if cancellation is not None and cancellation.is_cancelled():
        return True
    return deadline is not None and time.monotonic() >= deadline


def _operation_deadline(timeout: float, deadline: float | None) -> float:
    local_deadline = time.monotonic() + max(0.0, timeout)
    if deadline is None:
        return local_deadline
    return min(deadline, local_deadline)


@runtime_checkable
class HealthOperations(Protocol):
    """Small injectable seam for all filesystem/process/socket operations."""

    def observe_path(
        self,
        path: Path,
        *,
        include_sha256: bool = False,
        max_bytes: int = MAX_FILE_BYTES,
        max_seconds: float = MAX_CHECK_SECONDS,
    ) -> PathObservation:
        ...

    def read_regular_file(self, path: Path, *, max_bytes: int) -> bytes:
        ...

    def run(
        self,
        argv: Sequence[str],
        *,
        timeout: float,
        max_output_bytes: int,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> CommandObservation:
        ...

    def run_descriptor(
        self,
        path: Path,
        argv: Sequence[str],
        *,
        timeout: float,
        max_output_bytes: int,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> CommandObservation:
        ...

    def socket_reachable(self, endpoint: SocketEndpoint, timeout: float) -> SocketObservation:
        ...


def _finite_nonnegative(name: str, value: int | float, hard_limit: int | float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be a finite non-negative number")
    if not math.isfinite(value) or value < 0:
        raise ValueError(f"{name} must be a finite non-negative number")
    if value > hard_limit:
        raise ValueError(f"{name} exceeds hard limit {hard_limit:g}")
    return float(value)


def _nonnegative_integer(name: str, value: int, hard_limit: int) -> int:
    if type(value) is not int or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    if value > hard_limit:
        raise ValueError(f"{name} exceeds hard limit {hard_limit}")
    return value


def _safe_text(value: object) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    if isinstance(value, str):
        return value
    if value is None:
        return ""
    return ""


def _redact_text(value: object) -> str:
    text = _safe_text(value)
    text = _BEARER_RE.sub(rf"\1{_SECRET_REPLACEMENT}", text)
    text = _SECRET_ASSIGNMENT_RE.sub(rf"\1{_SECRET_REPLACEMENT}", text)
    text = _SECRET_FLAG_RE.sub(rf"\1{_SECRET_REPLACEMENT}", text)
    return text


def _bounded_text(value: object, limit: int) -> tuple[str, bool]:
    """Redact and then byte-bound one untrusted text value."""

    text = _redact_text(value)
    raw = text.encode("utf-8", errors="replace")
    if len(raw) <= limit:
        return text, False
    return raw[:limit].decode("utf-8", errors="ignore"), True


def _bounded_outputs(stdout: object, stderr: object, limit: int) -> tuple[str, str, bool]:
    """Bound both streams by one combined byte budget."""

    stdout_text = _redact_text(stdout)
    stderr_text = _redact_text(stderr)
    stdout_raw = stdout_text.encode("utf-8", errors="replace")
    stderr_raw = stderr_text.encode("utf-8", errors="replace")
    stdout_bytes = stdout_raw[:limit]
    remaining = max(0, limit - len(stdout_bytes))
    stderr_bytes = stderr_raw[:remaining]
    truncated = len(stdout_bytes) != len(stdout_raw) or len(stderr_bytes) != len(stderr_raw)
    return (
        stdout_bytes.decode("utf-8", errors="ignore"),
        stderr_bytes.decode("utf-8", errors="ignore"),
        truncated,
    )


def _bounded_detail(value: object) -> str | None:
    text = _redact_text(value)
    if not text:
        return None
    raw = text.encode("utf-8", errors="replace")
    if len(raw) > MAX_DETAIL_BYTES:
        return raw[:MAX_DETAIL_BYTES].decode("utf-8", errors="ignore")
    return text


def _normalise_error_code(value: object) -> str | None:
    if value is None:
        return None
    if isinstance(value, Enum):
        value = value.value
    if not isinstance(value, str):
        return "operation_error"
    code = value.strip().lower().replace("-", "_")
    if not code:
        return None
    return code if _REASON_CODE_RE.fullmatch(code) else "operation_error"


def _terminate_process(process: subprocess.Popen[bytes]) -> None:
    """Kill a bounded process and its same-session descendants."""

    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (AttributeError, OSError):
        try:
            process.kill()
        except (OSError, ProcessLookupError):
            pass


def _wait_process(process: subprocess.Popen[bytes], timeout: float) -> None:
    timeout = max(0.0, timeout)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        _terminate_process(process)
        try:
            process.wait(timeout=min(0.1, max(timeout, 0.01)))
        except (subprocess.TimeoutExpired, OSError):
            # The worker is daemonised by the executor and the descriptors are
            # closed below.  Do not turn cleanup into an unbounded wait.
            pass


def _run_bounded_process(
    argv: tuple[str, ...],
    *,
    timeout: float,
    max_output_bytes: int,
    executable_fd: int | None = None,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> CommandObservation:
    """Run argv without a shell while bounding time, output, and cleanup."""

    started = time.monotonic()
    operation_deadline = _operation_deadline(timeout, deadline)
    process: subprocess.Popen[bytes] | None = None
    selector: selectors.BaseSelector | None = None
    stdout = bytearray()
    stderr = bytearray()
    timed_out = False
    output_limited = False
    error_code: str | None = None
    error_detail: str | None = None

    if _operation_cancelled(operation_deadline, cancellation):
        return CommandObservation(
            argv,
            timed_out=True,
            error_code="timeout",
            error_detail="process launch was cancelled before its deadline",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )

    try:
        # Keep this check directly adjacent to Popen.  The descriptor and
        # snapshot stages use the same absolute deadline, so an expired
        # operation cannot start a probe after Doctor has timed out.
        if _operation_cancelled(operation_deadline, cancellation):
            return CommandObservation(
                argv,
                timed_out=True,
                error_code="timeout",
                error_detail="process launch was cancelled before its deadline",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        if executable_fd is None:
            process = subprocess.Popen(
                argv,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                shell=False,
                close_fds=True,
                start_new_session=True,
                env=dict(_SANITIZED_ENVIRONMENT),
            )
        else:
            # Python exposes fd-backed exec through ``os.execve`` on some
            # POSIX builds but does not expose it as a Popen argument.  The
            # proc-fd executable path is safe here because the descriptor is
            # opened with O_NOFOLLOW, kept alive with pass_fds, and never
            # resolved through PATH.
            process = subprocess.Popen(
                argv,
                executable=f"/proc/self/fd/{executable_fd}",
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                shell=False,
                close_fds=True,
                pass_fds=(executable_fd,),
                start_new_session=True,
                env=dict(_SANITIZED_ENVIRONMENT),
            )
    except FileNotFoundError:
        return CommandObservation(
            argv,
            error_code=("descriptor_execution_unavailable" if executable_fd is not None else "executable_missing"),
            error_detail="FileNotFoundError",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except PermissionError:
        return CommandObservation(
            argv,
            error_code="permission_denied",
            error_detail="PermissionError",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except OSError as exc:
        return CommandObservation(
            argv,
            error_code="io_error",
            error_detail=type(exc).__name__,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )

    try:
        selector = selectors.DefaultSelector()
        if process.stdout is not None:
            selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        if process.stderr is not None:
            selector.register(process.stderr, selectors.EVENT_READ, "stderr")

        while selector.get_map():
            remaining_time = operation_deadline - time.monotonic()
            if remaining_time <= 0 or _operation_cancelled(operation_deadline, cancellation):
                timed_out = True
                _terminate_process(process)
                break
            events = selector.select(timeout=min(remaining_time, 0.05))
            if not events:
                continue
            for key, _ in events:
                if _operation_cancelled(operation_deadline, cancellation):
                    timed_out = True
                    _terminate_process(process)
                    break
                try:
                    chunk = os.read(key.fd, 64 * 1024)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    selector.unregister(key.fileobj)
                    continue
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                used = len(stdout) + len(stderr)
                remaining_output = max_output_bytes - used
                if remaining_output <= 0:
                    output_limited = True
                    _terminate_process(process)
                    break
                if len(chunk) > remaining_output:
                    chunk = chunk[:remaining_output]
                    output_limited = True
                if key.data == "stdout":
                    stdout.extend(chunk)
                else:
                    stderr.extend(chunk)
                if output_limited:
                    _terminate_process(process)
                    break
            if timed_out or output_limited:
                break

        if timed_out or output_limited:
            _wait_process(process, 0.1)
        elif process.poll() is None:
            # Streams can be closed by a still-running child.  Wait for the
            # declared deadline instead of turning that case into a synthetic
            # non-zero failure.
            remaining = max(0.0, operation_deadline - time.monotonic())
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                timed_out = True
                _terminate_process(process)
                _wait_process(process, 0.1)
    except BaseException as exc:
        # An unexpected selector/pipe failure must not leave a child alive
        # after the bounded operation has been reported to Doctor.
        error_code = "operation_error"
        error_detail = type(exc).__name__
    finally:
        if process.poll() is None:
            _terminate_process(process)
            _wait_process(process, MAX_WORKER_CLEANUP_SECONDS)
        if selector is not None:
            try:
                selector.close()
            except (OSError, ValueError):
                pass
        try:
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
        except (OSError, ValueError):
            if error_code is None:
                error_code = "operation_error"
                error_detail = "pipe_close_error"

    return CommandObservation(
        argv,
        returncode=process.returncode,
        stdout=bytes(stdout),
        stderr=bytes(stderr),
        timed_out=timed_out,
        output_limited=output_limited,
        error_code=error_code,
        error_detail=error_detail,
        duration_ms=(time.monotonic() - started) * 1000.0,
    )


class ReadOnlyHealthOperations:
    """Default filesystem/process/socket implementation for Doctor."""

    def observe_path(
        self,
        path: Path,
        *,
        include_sha256: bool = False,
        max_bytes: int = MAX_FILE_BYTES,
        max_seconds: float = MAX_CHECK_SECONDS,
    ) -> PathObservation:
        return observe_path(
            Path(path),
            include_sha256=include_sha256,
            max_bytes=max_bytes,
            max_seconds=max_seconds,
        )

    def read_regular_file(self, path: Path, *, max_bytes: int) -> bytes:
        return read_regular_file(Path(path), max_bytes=max_bytes, hard_limit=MAX_FILE_BYTES)

    def run(
        self,
        argv: Sequence[str],
        *,
        timeout: float,
        max_output_bytes: int,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> CommandObservation:
        command = tuple(argv)
        if not command or not _trusted_absolute_executable(command[0]):
            return CommandObservation(
                command,
                error_code="invalid_executable",
                error_detail="absolute executable identity required",
            )
        if not _approved_artifactless_arguments(command):
            return CommandObservation(
                command,
                error_code="invalid_command",
                error_detail="executable identity or argument schema is not approved",
            )
        return self.run_descriptor(
            Path(command[0]),
            command,
            timeout=timeout,
            max_output_bytes=max_output_bytes,
            deadline=deadline,
            cancellation=cancellation,
        )

    def run_descriptor(
        self,
        path: Path,
        argv: Sequence[str],
        *,
        timeout: float,
        max_output_bytes: int,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> CommandObservation:
        command = tuple(argv)
        if not command or command[0] != os.fspath(path):
            return CommandObservation(
                command,
                error_code="invalid_executable",
                error_detail="descriptor path and argv identity differ",
            )
        if _normalise_argv(command) is None:
            return CommandObservation(
                command,
                error_code="invalid_command",
                error_detail="structured argv rejected",
            )
        if command[0] in _APPROVED_ARTIFACTLESS_EXECUTABLES:
            approved = _approved_artifactless_arguments(command)
        else:
            approved = _approved_probe_arguments(command, version=False)
        if not approved:
            return CommandObservation(
                command,
                error_code="invalid_command",
                error_detail="executable identity or argument schema is not approved",
            )
        return _run_bounded_descriptor(
            Path(path),
            command,
            timeout=timeout,
            max_output_bytes=max_output_bytes,
            deadline=deadline,
            cancellation=cancellation,
        )

    def socket_reachable(self, endpoint: SocketEndpoint, timeout: float) -> SocketObservation:
        started = time.monotonic()
        timeout = min(max(0.0, timeout), MAX_SOCKET_SECONDS)
        if endpoint.kind == "unix" and endpoint.path:
            if not endpoint.path.startswith("/") or len(os.fsencode(endpoint.path)) > MAX_SOCKET_PATH_BYTES:
                return SocketObservation(
                    False,
                    error_code="invalid_endpoint",
                    duration_ms=(time.monotonic() - started) * 1000.0,
                )
            family = socket.AF_UNIX
            address: object = endpoint.path
        elif endpoint.kind == "tcp" and endpoint.host and endpoint.port:
            if endpoint.host not in {"localhost", "127.0.0.1", "::1"}:
                return SocketObservation(
                    False,
                    error_code="remote_endpoint_forbidden",
                    duration_ms=(time.monotonic() - started) * 1000.0,
                )
            if endpoint.host == "localhost":
                host = "127.0.0.1"
            else:
                host = endpoint.host
            family = socket.AF_INET6 if ":" in host else socket.AF_INET
            address = (host, endpoint.port)
        else:
            return SocketObservation(
                False,
                error_code="invalid_endpoint",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )

        connection: socket.socket | None = None
        try:
            connection = socket.socket(family, socket.SOCK_STREAM)
            connection.settimeout(timeout)
            connection.connect(address)
            return SocketObservation(
                True,
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        except ConnectionRefusedError:
            return SocketObservation(
                False,
                error_code="connection_refused",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        except FileNotFoundError:
            return SocketObservation(
                False,
                error_code="endpoint_missing",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        except TimeoutError:
            return SocketObservation(
                False,
                error_code="timeout",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        except PermissionError:
            return SocketObservation(
                False,
                error_code="permission_denied",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        except OSError as exc:
            return SocketObservation(
                False,
                error_code="socket_error",
                error_detail=type(exc).__name__,
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        finally:
            if connection is not None:
                connection.close()


DefaultHealthOperations = ReadOnlyHealthOperations


@dataclass(frozen=True)
class HealthCheckResult:
    """One bounded observation with machine-readable status and reason."""

    check_id: str
    component_id: str | None
    check: str | None
    status: HealthStatus
    reason_code: str
    detail: str | None = None
    duration_ms: float = 0.0
    stdout: str = ""
    stderr: str = ""
    value: object | None = None
    cached: bool = False
    output_truncated: bool = False

    @property
    def state(self) -> HealthStatus:
        return self.status

    @property
    def outcome(self) -> HealthStatus:
        return self.status

    @property
    def reason(self) -> str:
        return self.reason_code

    @property
    def elapsed_ms(self) -> float:
        return self.duration_ms

    @property
    def healthy(self) -> bool:
        return self.status is HealthStatus.PASS

    @property
    def satisfied(self) -> bool:
        return self.status in {HealthStatus.PASS, HealthStatus.NOT_APPLICABLE}

    def to_dict(self) -> dict[str, object]:
        return {
            "check_id": self.check_id,
            "component_id": self.component_id,
            "check": self.check,
            "status": self.status.value,
            "reason_code": self.reason_code,
            "detail": self.detail,
            "duration_ms": self.duration_ms,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "value": self.value,
            "cached": self.cached,
            "output_truncated": self.output_truncated,
        }


HealthCheckExecutionResult = HealthCheckResult


@dataclass(frozen=True)
class HealthCheckReport:
    """A complete, bounded run.  It is iterable for lightweight callers."""

    results: tuple[HealthCheckResult, ...]
    elapsed_ms: float
    budget_exhausted: bool = False
    check_ids: tuple[str, ...] = ()
    policy_check_ids: tuple[str, ...] = ()
    cache_hits: int = 0

    def __iter__(self):
        return iter(self.results)

    def __len__(self) -> int:
        return len(self.results)

    def __getitem__(self, index):
        return self.results[index]

    def result_for(self, check_id: str) -> HealthCheckResult:
        for result in self.results:
            if result.check_id == check_id:
                return result
        raise KeyError(check_id)

    @property
    def outcomes(self) -> tuple[HealthCheckResult, ...]:
        return self.results

    @property
    def passed(self) -> tuple[HealthCheckResult, ...]:
        return tuple(item for item in self.results if item.status is HealthStatus.PASS)

    @property
    def failed(self) -> tuple[HealthCheckResult, ...]:
        return tuple(item for item in self.results if item.status is HealthStatus.FAIL)

    @property
    def unknown(self) -> tuple[HealthCheckResult, ...]:
        return tuple(item for item in self.results if item.status is HealthStatus.UNKNOWN)

    def to_dict(self) -> dict[str, object]:
        counts = {status.value: 0 for status in HealthStatus}
        for result in self.results:
            counts[result.status.value] += 1
        return {
            "results": [result.to_dict() for result in self.results],
            "elapsed_ms": self.elapsed_ms,
            "budget_exhausted": self.budget_exhausted,
            "check_ids": list(self.check_ids),
            "policy_check_ids": list(self.policy_check_ids),
            "cache_hits": self.cache_hits,
            "counts": counts,
        }


HealthCheckExecutionReport = HealthCheckReport
HealthCheckRun = HealthCheckReport


@dataclass(frozen=True)
class _CheckPayload:
    status: HealthStatus
    reason_code: str
    detail: str | None = None
    stdout: str = ""
    stderr: str = ""
    value: object | None = None
    output_truncated: bool = False


def _payload(
    status: HealthStatus,
    reason_code: str,
    *,
    detail: object = None,
    stdout: object = "",
    stderr: object = "",
    value: object = None,
    output_truncated: bool = False,
) -> _CheckPayload:
    output_stdout, output_stderr, bounded = _bounded_outputs(stdout, stderr, MAX_OUTPUT_BYTES)
    return _CheckPayload(
        status,
        _normalise_error_code(reason_code) or "operation_error",
        detail=_bounded_detail(detail),
        stdout=output_stdout,
        stderr=output_stderr,
        value=value,
        output_truncated=output_truncated or bounded,
    )


def _normalise_argv(raw: object) -> tuple[str, ...] | None:
    if isinstance(raw, (str, bytes)) or not isinstance(raw, Sequence):
        return None
    if not raw or len(raw) > MAX_ARGUMENTS:
        return None
    result: list[str] = []
    total_bytes = 0
    for item in raw:
        if type(item) is not str or not item or "\x00" in item:
            return None
        try:
            encoded_length = len(item.encode("utf-8", errors="surrogateescape"))
        except UnicodeError:
            return None
        total_bytes += encoded_length
        if encoded_length > MAX_ARGUMENT_BYTES or total_bytes > MAX_ARGUMENT_BYTES:
            return None
        result.append(item)
    return tuple(result)


def _trusted_absolute_executable(value: object) -> bool:
    """Accept only one concrete absolute executable identity.

    A relative name would make ``Popen`` consult inherited ``PATH``.  Dot
    segments, duplicate separators, and trailing separators are rejected as
    ambiguous spellings rather than normalized into an authorization decision.
    """

    if not isinstance(value, str) or not value or "\x00" in value:
        return False
    if not value.startswith("/") or value.startswith("//"):
        return False
    if any(part in {"", ".", ".."} for part in value.split("/")[1:]):
        return False
    try:
        return len(value.encode("utf-8", errors="surrogateescape")) <= MAX_ARGUMENT_BYTES
    except UnicodeError:
        return False


def _approved_artifactless_arguments(argv: tuple[str, ...]) -> bool:
    """Validate an artifact-less command against an exact identity policy."""

    if not argv:
        return False
    schema = _APPROVED_ARTIFACTLESS_EXECUTABLES.get(argv[0])
    if schema == "no_args":
        return len(argv) == 1
    if schema == "printf":
        # GNU printf has no read-only guarantee for option forms such as -v.
        # Allow only literal format/argument data and never an option.
        return len(argv) >= 2 and all(not value.startswith("-") for value in argv[1:])
    if schema == "sleep":
        return len(argv) == 2 and re.fullmatch(r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?", argv[1]) is not None
    return False


def _approved_probe_arguments(argv: tuple[str, ...], *, version: bool) -> bool:
    """Validate the narrow argv grammar shared by declared ELF artifacts."""

    tail = argv[1:]
    if len(tail) > 1:
        return False
    if version:
        return len(tail) == 1 and tail[0] in _APPROVED_VERSION_FLAGS
    return not tail or tail[0] in _APPROVED_PROBE_FLAGS


def _authorise_command_argv(
    argv: tuple[str, ...],
    *,
    artifact_bound: bool,
    version: bool,
) -> bool:
    """Apply positive command identity and argument authorization."""

    if artifact_bound:
        if argv[0] in _APPROVED_ARTIFACTLESS_EXECUTABLES:
            return _approved_artifactless_arguments(argv)
        return _approved_probe_arguments(argv, version=version)
    return _approved_artifactless_arguments(argv)


def _command_tail(args: Mapping[str, Any], *, version: bool) -> tuple[object, ...] | None:
    tail_keys = [key for key in ("version_argv", "arguments", "args") if key in args]
    if len(tail_keys) > 1:
        return None
    if not tail_keys:
        raw = ()
    elif tail_keys[0] == "version_argv" and not version:
        return None
    else:
        raw = args[tail_keys[0]]
    if raw is None:
        return ()
    if isinstance(raw, (str, bytes)) or not isinstance(raw, Sequence):
        return None
    return tuple(raw)


def _command_from_args(
    args: Mapping[str, Any],
    *,
    version: bool,
    artifact_executable: str | None = None,
) -> tuple[str, ...] | None:
    if any(
        key in args
        for key in (
            "shell_command",
            "command_line",
            "env",
            "cwd",
            "environment",
            "working_directory",
        )
    ):
        return None
    if "shell" in args and args["shell"] is not False:
        return None

    command_keys = [key for key in ("argv", "command") if key in args]
    executable_keys = [key for key in ("executable", "program") if key in args]
    if len(executable_keys) > 1:
        return None
    executable_key = executable_keys[0] if executable_keys else None
    if len(command_keys) > 1 or (command_keys and executable_key is not None):
        return None
    if command_keys:
        if any(key in args for key in ("version_argv", "arguments", "args")):
            return None
        argv = _normalise_argv(args[command_keys[0]])
        if argv is None or (artifact_executable is not None and argv[0] != artifact_executable):
            return None
        return argv
    if executable_key is not None:
        executable = args[executable_key]
        if type(executable) is not str:
            return None
        tail = _command_tail(args, version=version)
        if tail is None:
            return None
        argv = _normalise_argv((executable, *tail))
        if argv is None or (artifact_executable is not None and argv[0] != artifact_executable):
            return None
        return argv
    if artifact_executable is None:
        return None
    tail = _command_tail(args, version=version)
    if tail is None:
        return None
    return _normalise_argv((artifact_executable, *tail))


def _descriptor_execution_available() -> bool:
    """Return whether this Python process can execute an already-open fd."""

    execve = getattr(os, "execve", None)
    supports_fd = getattr(os, "supports_fd", ())
    return (
        callable(execve)
        and execve in supports_fd
        and os.path.isdir("/proc/self/fd")
    )


def _relocate_descriptor(
    descriptor: int,
    *,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> tuple[int | None, str | None]:
    """Move a passed executable descriptor out of the stdio range."""

    if descriptor >= MIN_PASSTHROUGH_FD:
        if _operation_cancelled(deadline, cancellation):
            try:
                os.close(descriptor)
            except OSError:
                pass
            return None, "timeout"
        return descriptor, None
    duplicate_function = getattr(fcntl, "F_DUPFD_CLOEXEC", None)
    if duplicate_function is None:
        duplicate_function = getattr(fcntl, "F_DUPFD", None)
    if duplicate_function is None:
        try:
            os.close(descriptor)
        except OSError:
            return descriptor, "descriptor_relocation_unavailable"
        return None, "descriptor_relocation_unavailable"
    if _operation_cancelled(deadline, cancellation):
        try:
            os.close(descriptor)
        except OSError:
            pass
        return None, "timeout"
    try:
        relocated = fcntl.fcntl(descriptor, duplicate_function, MIN_PASSTHROUGH_FD)
    except (OSError, ValueError, TypeError):
        try:
            os.close(descriptor)
        except OSError:
            return descriptor, "descriptor_relocation_failed"
        return None, "descriptor_relocation_failed"
    if _operation_cancelled(deadline, cancellation):
        try:
            os.close(descriptor)
        except OSError:
            try:
                os.close(relocated)
            except OSError:
                pass
            return relocated, "timeout"
        try:
            os.close(relocated)
        except OSError:
            return relocated, "timeout"
        return None, "timeout"
    try:
        os.close(descriptor)
    except OSError:
        try:
            os.close(relocated)
        except OSError:
            pass
        return descriptor, "descriptor_relocation_failed"
    return relocated, None


def _authorise_executable_descriptor(
    descriptor: int,
    metadata: os.stat_result,
    *,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> str | None:
    """Authorize a descriptor by mode and executable file format.

    Script support is deliberately absent: without a signed/content-bound
    script contract, a shebang would be an interpreter-selection escape hatch.
    """

    if not stat.S_ISREG(metadata.st_mode):
        return "artifact_not_regular"
    if not metadata.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
        return "artifact_not_executable"
    if not metadata.st_mode & (stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH):
        return "artifact_not_readable"
    if metadata.st_mode & (stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX):
        return "special_mode_forbidden"
    if _operation_cancelled(deadline, cancellation):
        return "timeout"
    try:
        header = os.pread(descriptor, EXECUTABLE_HEADER_BYTES, 0)
    except (AttributeError, OSError):
        return "artifact_not_readable"
    if _operation_cancelled(deadline, cancellation):
        return "timeout"
    if header.startswith(b"#!"):
        return "unsupported_executable"
    if header != b"\x7fELF":
        return "unsupported_executable"
    return None


def _read_descriptor_content(
    descriptor: int,
    *,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> tuple[bytes | None, str | None]:
    """Read one bounded descriptor snapshot without following its path."""

    pread = getattr(os, "pread", None)
    if not callable(pread):
        return None, "immutable_snapshot_unavailable"
    content = bytearray()
    offset = 0
    try:
        while True:
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            chunk = pread(descriptor, EXECUTABLE_SNAPSHOT_CHUNK_BYTES, offset)
            if not isinstance(chunk, bytes):
                return None, "immutable_snapshot_unavailable"
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            if not chunk:
                return bytes(content), None
            if len(content) + len(chunk) > MAX_EXECUTABLE_SNAPSHOT_BYTES:
                return None, "artifact_snapshot_too_large"
            content.extend(chunk)
            offset += len(chunk)
    except (AttributeError, OSError):
        return None, "immutable_snapshot_unavailable"


def _same_executable_metadata(left: os.stat_result, right: os.stat_result) -> bool:
    """Compare the source identity and mutation-relevant metadata."""

    return (
        left.st_dev == right.st_dev
        and left.st_ino == right.st_ino
        and left.st_mode == right.st_mode
        and left.st_size == right.st_size
        and left.st_mtime_ns == right.st_mtime_ns
        and left.st_ctime_ns == right.st_ctime_ns
    )


def _snapshot_executable_descriptor(
    descriptor: int,
    metadata: os.stat_result,
    *,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> tuple[int | None, str | None]:
    """Create a private immutable/read-only copy, re-authorize, and relocate it."""

    if _operation_cancelled(deadline, cancellation):
        return None, "timeout"
    first_content, error_code = _read_descriptor_content(
        descriptor,
        deadline=deadline,
        cancellation=cancellation,
    )
    if error_code is not None or first_content is None:
        return None, error_code or "immutable_snapshot_unavailable"
    second_content, error_code = _read_descriptor_content(
        descriptor,
        deadline=deadline,
        cancellation=cancellation,
    )
    if error_code is not None or second_content is None:
        return None, error_code or "immutable_snapshot_unavailable"
    if _operation_cancelled(deadline, cancellation):
        return None, "timeout"
    try:
        current_metadata = os.fstat(descriptor)
    except (AttributeError, OSError):
        return None, "immutable_snapshot_unavailable"
    if _operation_cancelled(deadline, cancellation):
        return None, "timeout"
    if (
        first_content != second_content
        or len(first_content) != metadata.st_size
        or not _same_executable_metadata(metadata, current_metadata)
    ):
        return None, "artifact_snapshot_unstable"

    memfd_create = getattr(os, "memfd_create", None)
    add_seals = getattr(fcntl, "F_ADD_SEALS", None)
    get_seals = getattr(fcntl, "F_GET_SEALS", None)
    allow_sealing = getattr(os, "MFD_ALLOW_SEALING", None)
    close_on_exec = getattr(os, "MFD_CLOEXEC", None)
    seal_names = ("F_SEAL_SEAL", "F_SEAL_SHRINK", "F_SEAL_GROW", "F_SEAL_WRITE")
    seal_values: tuple[int, ...] = tuple(
        value for value in (getattr(fcntl, name, None) for name in seal_names) if isinstance(value, int)
    )
    use_memfd = (
        callable(memfd_create)
        and isinstance(add_seals, int)
        and isinstance(get_seals, int)
        and isinstance(allow_sealing, int)
        and isinstance(close_on_exec, int)
        and len(seal_values) == len(seal_names)
    )
    tmpfile_flag = getattr(os, "O_TMPFILE", None)
    tmpfile_close_on_exec = getattr(os, "O_CLOEXEC", None)
    use_tmpfile = isinstance(tmpfile_flag, int) and isinstance(tmpfile_close_on_exec, int)
    if not use_memfd and not use_tmpfile:
        return None, "immutable_snapshot_unavailable"

    add_seals_value = cast(int, add_seals)
    get_seals_value = cast(int, get_seals)
    allow_sealing_value = cast(int, allow_sealing)
    close_on_exec_value = cast(int, close_on_exec)
    tmpfile_flag_value = cast(int, tmpfile_flag)
    tmpfile_close_on_exec_value = cast(int, tmpfile_close_on_exec)
    memfd_create_function = cast(Callable[[str, int], int], memfd_create) if use_memfd else None
    snapshot_descriptor: int | None = None
    keep_open = False
    try:
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        if memfd_create_function is not None:
            snapshot_descriptor = memfd_create_function(
                "realmheart-doctor-executable",
                allow_sealing_value | close_on_exec_value,
            )
        else:
            for directory in ("/tmp", "/var/tmp", "/dev/shm"):
                if _operation_cancelled(deadline, cancellation):
                    return None, "timeout"
                try:
                    snapshot_descriptor = os.open(
                        directory,
                        tmpfile_flag_value | os.O_RDWR | tmpfile_close_on_exec_value,
                        0o700,
                    )
                    break
                except OSError:
                    continue
        if type(snapshot_descriptor) is not int:
            return None, "immutable_snapshot_unavailable"
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        write_offset = 0
        while write_offset < len(first_content):
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            written = os.write(
                snapshot_descriptor,
                first_content[write_offset : write_offset + EXECUTABLE_SNAPSHOT_CHUNK_BYTES],
            )
            if type(written) is not int or written <= 0:
                return None, "immutable_snapshot_unavailable"
            write_offset += written
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        os.fchmod(snapshot_descriptor, stat.S_IMODE(metadata.st_mode))
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        if memfd_create_function is not None:
            seal_mask = 0
            for value in seal_values:
                seal_mask |= value
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            fcntl.fcntl(snapshot_descriptor, add_seals_value, seal_mask)
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            observed_seals = fcntl.fcntl(snapshot_descriptor, get_seals_value)
            if type(observed_seals) is not int or observed_seals & seal_mask != seal_mask:
                return None, "immutable_snapshot_unavailable"
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
        else:
            readonly_descriptor: int | None = None
            try:
                if _operation_cancelled(deadline, cancellation):
                    return None, "timeout"
                readonly_descriptor = os.open(
                    f"/proc/self/fd/{snapshot_descriptor}",
                    os.O_RDONLY | tmpfile_close_on_exec_value,
                )
                if _operation_cancelled(deadline, cancellation):
                    try:
                        os.close(readonly_descriptor)
                    except OSError:
                        pass
                    readonly_descriptor = None
                    return None, "timeout"
                os.close(snapshot_descriptor)
            except OSError:
                if readonly_descriptor is not None:
                    try:
                        os.close(readonly_descriptor)
                    except OSError:
                        pass
                raise
            snapshot_descriptor = readonly_descriptor
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        snapshot_metadata = os.fstat(snapshot_descriptor)
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        error_code = _authorise_executable_descriptor(
            snapshot_descriptor,
            snapshot_metadata,
            deadline=deadline,
            cancellation=cancellation,
        )
        if error_code is not None:
            return None, error_code
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        snapshot_descriptor, error_code = _relocate_descriptor(
            snapshot_descriptor,
            deadline=deadline,
            cancellation=cancellation,
        )
        if error_code is not None or snapshot_descriptor is None:
            return None, error_code or "descriptor_relocation_failed"
        keep_open = True
        return snapshot_descriptor, None
    except (AttributeError, OSError, PermissionError, TypeError, ValueError):
        return None, "immutable_snapshot_unavailable"
    finally:
        if not keep_open and snapshot_descriptor is not None:
            try:
                os.close(snapshot_descriptor)
            except OSError:
                pass


def _open_executable_descriptor(
    path: Path,
    *,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> tuple[int | None, str | None]:
    """Open and authorize one regular ELF executable without symlink follows."""

    path_text = os.fspath(path)
    if _operation_cancelled(deadline, cancellation):
        return None, "timeout"
    if not _trusted_absolute_executable(path_text):
        return None, "invalid_executable"
    if not _descriptor_execution_available():
        return None, "descriptor_execution_unavailable"
    nofollow = getattr(os, "O_NOFOLLOW", None)
    nonblocking = getattr(os, "O_NONBLOCK", None)
    directory_flag = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or nonblocking is None or directory_flag is None:
        return None, "descriptor_execution_unavailable"
    cloexec = getattr(os, "O_CLOEXEC", None)
    common_flags = os.O_RDONLY | nofollow | directory_flag
    if cloexec is not None:
        common_flags |= cloexec
    file_flags = os.O_RDONLY | nofollow | nonblocking
    if cloexec is not None:
        file_flags |= cloexec
    components = path_text.split("/")[1:]
    if not components:
        return None, "invalid_executable"
    parent_descriptor: int | None = None
    descriptor: int | None = None
    try:
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        parent_descriptor = os.open("/", common_flags)
        for component in components[:-1]:
            if _operation_cancelled(deadline, cancellation):
                return None, "timeout"
            child_descriptor = os.open(component, common_flags, dir_fd=parent_descriptor)
            if _operation_cancelled(deadline, cancellation):
                os.close(child_descriptor)
                return None, "timeout"
            try:
                os.close(parent_descriptor)
            except OSError:
                os.close(child_descriptor)
                raise
            parent_descriptor = child_descriptor
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        descriptor = os.open(components[-1], file_flags, dir_fd=parent_descriptor)
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
    except FileNotFoundError:
        error_code = "executable_missing"
    except PermissionError:
        error_code = "permission_denied"
    except (NotImplementedError, TypeError):
        error_code = "descriptor_execution_unavailable"
    except OSError as exc:
        if exc.errno == errno.ELOOP:
            error_code = "symlink_forbidden"
        else:
            error_code = "descriptor_open_failed"
    else:
        error_code = None
    finally:
        if parent_descriptor is not None:
            try:
                os.close(parent_descriptor)
            except OSError:
                pass
    if error_code is not None or descriptor is None:
        if descriptor is not None:
            os.close(descriptor)
        return None, error_code or "descriptor_open_failed"

    try:
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        metadata = os.fstat(descriptor)
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        error_code = _authorise_executable_descriptor(
            descriptor,
            metadata,
            deadline=deadline,
            cancellation=cancellation,
        )
        if error_code is not None:
            return None, error_code
        if _operation_cancelled(deadline, cancellation):
            return None, "timeout"
        snapshot_descriptor, error_code = _snapshot_executable_descriptor(
            descriptor,
            metadata,
            deadline=deadline,
            cancellation=cancellation,
        )
        if error_code is not None or snapshot_descriptor is None:
            return None, error_code or "immutable_snapshot_unavailable"
        return snapshot_descriptor, None
    except PermissionError:
        return None, "permission_denied"
    except OSError:
        return None, "descriptor_observation_failed"
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass


def _run_bounded_descriptor(
    path: Path,
    argv: tuple[str, ...],
    *,
    timeout: float,
    max_output_bytes: int,
    deadline: float | None = None,
    cancellation: _CancellationToken | None = None,
) -> CommandObservation:
    """Run one executable through a descriptor-bound process seam."""

    started = time.monotonic()
    operation_deadline = _operation_deadline(timeout, deadline)
    if _operation_cancelled(operation_deadline, cancellation):
        return CommandObservation(
            argv,
            timed_out=True,
            error_code="timeout",
            error_detail="descriptor operation was cancelled before it started",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    descriptor, error_code = _open_executable_descriptor(
        path,
        deadline=operation_deadline,
        cancellation=cancellation,
    )
    if error_code is not None or descriptor is None:
        return CommandObservation(
            argv,
            error_code=error_code or "descriptor_execution_unavailable",
            error_detail=error_code,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    try:
        if _operation_cancelled(operation_deadline, cancellation):
            return CommandObservation(
                argv,
                timed_out=True,
                error_code="timeout",
                error_detail="descriptor operation expired before process launch",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        return _run_bounded_process(
            argv,
            timeout=timeout,
            max_output_bytes=max_output_bytes,
            executable_fd=descriptor,
            deadline=operation_deadline,
            cancellation=cancellation,
        )
    finally:
        os.close(descriptor)


def _coerce_command_observation(raw: object, argv: tuple[str, ...]) -> CommandObservation | None:
    if isinstance(raw, CommandObservation):
        candidate = raw
    elif isinstance(raw, subprocess.CompletedProcess):
        candidate = CommandObservation(
            argv,
            returncode=raw.returncode,
            stdout=raw.stdout or "",
            stderr=raw.stderr or "",
        )
    elif isinstance(raw, Mapping):
        candidate = CommandObservation(
            argv,
            returncode=raw.get("returncode"),
            stdout=raw.get("stdout", ""),
            stderr=raw.get("stderr", ""),
            timed_out=raw.get("timed_out", raw.get("timeout", False)),
            output_limited=raw.get("output_limited", raw.get("output_truncated", False)),
            error_code=raw.get("error_code"),
            error_detail=raw.get("error_detail"),
            duration_ms=raw.get("duration_ms", 0.0) or 0.0,
        )
    elif raw is None:
        return None
    else:
        returncode = getattr(raw, "returncode", None)
        if returncode is None and not hasattr(raw, "stdout") and not hasattr(raw, "stderr"):
            return None
        candidate = CommandObservation(
            argv,
            returncode=returncode,
            stdout=getattr(raw, "stdout", ""),
            stderr=getattr(raw, "stderr", ""),
            timed_out=getattr(raw, "timed_out", getattr(raw, "timeout", False)),
            output_limited=getattr(raw, "output_limited", getattr(raw, "output_truncated", False)),
            error_code=getattr(raw, "error_code", None),
            error_detail=getattr(raw, "error_detail", None),
            duration_ms=getattr(raw, "duration_ms", 0.0) or 0.0,
        )

    if candidate.returncode is not None and type(candidate.returncode) is not int:
        return None
    if not isinstance(candidate.stdout, (str, bytes)) or not isinstance(candidate.stderr, (str, bytes)):
        return None
    if type(candidate.timed_out) is not bool or type(candidate.output_limited) is not bool:
        return None
    if candidate.error_code is not None and not isinstance(candidate.error_code, str):
        return None
    if candidate.error_detail is not None and not isinstance(candidate.error_detail, (str, bytes)):
        return None
    if isinstance(candidate.duration_ms, bool) or not isinstance(candidate.duration_ms, (int, float)):
        return None
    if not math.isfinite(float(candidate.duration_ms)) or candidate.duration_ms < 0:
        return None
    return CommandObservation(
        argv,
        returncode=candidate.returncode,
        stdout=candidate.stdout,
        stderr=candidate.stderr,
        timed_out=candidate.timed_out,
        output_limited=candidate.output_limited,
        error_code=_normalise_error_code(candidate.error_code),
        error_detail=_safe_text(candidate.error_detail),
        duration_ms=float(candidate.duration_ms),
    )


def _coerce_socket_observation(raw: object) -> SocketObservation | None:
    if isinstance(raw, SocketObservation):
        candidate = raw
    elif isinstance(raw, bool):
        candidate = SocketObservation(raw)
    elif isinstance(raw, Mapping):
        if "reachable" in raw:
            reachable = raw["reachable"]
        elif "ok" in raw:
            reachable = raw["ok"]
        else:
            return None
        if type(reachable) is not bool:
            return None
        candidate = SocketObservation(
            reachable,
            error_code=raw.get("error_code"),
            error_detail=raw.get("error_detail"),
            duration_ms=raw.get("duration_ms", 0.0) or 0.0,
        )
    elif raw is None:
        return None
    elif hasattr(raw, "reachable") or hasattr(raw, "ok"):
        reachable = getattr(raw, "reachable", getattr(raw, "ok", None))
        if type(reachable) is not bool:
            return None
        candidate = SocketObservation(
            reachable,
            error_code=getattr(raw, "error_code", None),
            error_detail=getattr(raw, "error_detail", None),
            duration_ms=getattr(raw, "duration_ms", 0.0) or 0.0,
        )
    else:
        return None
    if type(candidate.reachable) is not bool:
        return None
    if candidate.error_code is not None and not isinstance(candidate.error_code, str):
        return None
    if candidate.error_detail is not None and not isinstance(candidate.error_detail, (str, bytes)):
        return None
    if isinstance(candidate.duration_ms, bool) or not isinstance(candidate.duration_ms, (int, float)):
        return None
    if not math.isfinite(float(candidate.duration_ms)) or candidate.duration_ms < 0:
        return None
    if candidate.reachable and candidate.error_code not in {None, ""}:
        return None
    return SocketObservation(
        candidate.reachable,
        error_code=_normalise_error_code(candidate.error_code),
        error_detail=_safe_text(candidate.error_detail),
        duration_ms=float(candidate.duration_ms),
    )


def _coerce_path_observation(raw: object) -> PathObservation | None:
    if isinstance(raw, PathObservation):
        candidate = raw
    elif isinstance(raw, Mapping):
        if "exists" not in raw:
            return None
        candidate = PathObservation(
            raw["exists"],
            raw.get("mode"),
            raw.get("filesystem_type"),
            raw.get("sha256"),
            raw.get("immutable_fingerprint"),
        )
    elif raw is not None and hasattr(raw, "exists"):
        candidate = PathObservation(
            getattr(raw, "exists"),
            getattr(raw, "mode", None),
            getattr(raw, "filesystem_type", None),
            getattr(raw, "sha256", None),
            getattr(raw, "immutable_fingerprint", None),
        )
    else:
        return None
    if type(candidate.exists) is not bool:
        return None
    if candidate.mode is not None and (type(candidate.mode) is not int or candidate.mode < 0):
        return None
    if candidate.filesystem_type is not None and candidate.filesystem_type not in {
        "file",
        "directory",
        "symlink",
        "other",
    }:
        return None
    if not candidate.exists and any(
        value is not None
        for value in (candidate.mode, candidate.filesystem_type, candidate.sha256, candidate.immutable_fingerprint)
    ):
        return None
    for digest in (candidate.sha256, candidate.immutable_fingerprint):
        if digest is not None and (
            not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest)
        ):
            return None
    return PathObservation(
        candidate.exists,
        candidate.mode,
        candidate.filesystem_type,
        candidate.sha256,
        candidate.immutable_fingerprint,
    )


def _operation_failure(exc: BaseException, *, default_code: str = "operation_error") -> _CheckPayload:
    if isinstance(exc, PermissionError):
        code = "permission_denied"
    elif isinstance(exc, FileNotFoundError):
        code = "executable_missing" if default_code == "operation_error" else default_code
    elif isinstance(exc, TimeoutError):
        code = "timeout"
    elif isinstance(exc, (FingerprintLimitExceeded,)):
        code = "observation_limit_exceeded"
    elif isinstance(exc, FingerprintObservationError):
        code = "observation_unavailable"
    elif isinstance(exc, (UnicodeError, json.JSONDecodeError)):
        code = "malformed_output"
    else:
        code = default_code
    return _payload(HealthStatus.UNKNOWN, code, detail=type(exc).__name__)


def _expected_filesystem_type(artifact_type: object) -> str | None:
    if artifact_type in {"directory", "asset"}:
        return "directory"
    if artifact_type in {"executable", "library", "file", "config", "service", "pam", "generated"}:
        return "file"
    return None


def _mode_value(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 8)
        except ValueError:
            return None
    return None


def _normalised_cache_value(
    value: object,
    *,
    _seen: set[int] | None = None,
    _depth: int = 0,
) -> object:
    if _depth > 32:
        return ["<depth>", type(value).__name__, id(value)]
    if _seen is None:
        _seen = set()
    if isinstance(value, Mapping):
        identity = id(value)
        if identity in _seen:
            return ["<cycle>", type(value).__name__, identity]
        _seen.add(identity)
        try:
            return {
                str(key): _normalised_cache_value(item, _seen=_seen, _depth=_depth + 1)
                for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
            }
        finally:
            _seen.remove(identity)
    if isinstance(value, (tuple, list)):
        identity = id(value)
        if identity in _seen:
            return ["<cycle>", type(value).__name__, identity]
        _seen.add(identity)
        try:
            return [
                _normalised_cache_value(item, _seen=_seen, _depth=_depth + 1)
                for item in value
            ]
        finally:
            _seen.remove(identity)
    if isinstance(value, (str, int, bool)) or value is None:
        return value
    if isinstance(value, float):
        return value if math.isfinite(value) else str(value)
    if isinstance(value, Path):
        return os.fspath(value)
    return type(value).__name__


def _cache_key(check_id: str, args: object) -> str:
    try:
        encoded = json.dumps(
            [check_id, _normalised_cache_value(args)],
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        )
        if len(encoded.encode("utf-8")) > MAX_ARGUMENT_BYTES:
            return json.dumps(
                [check_id, "arguments_sha256", hashlib.sha256(encoded.encode("utf-8")).hexdigest()],
                separators=(",", ":"),
            )
        return encoded
    except BaseException:
        # Invalid/cyclic injected specs still receive a structured UNKNOWN
        # result; the fallback key is run-local and never leaves the process.
        return f"{check_id}\0{type(args).__name__}\0{id(args)}"


def _normalise_identity(value: object) -> str:
    if not isinstance(value, str):
        return ""
    text = re.sub(r"[^a-z0-9]+", "", Path(value).name.lower())
    return re.sub(r"\d+$", "", text)


def _version_prefix_matches(prefix: str, identity: str) -> bool:
    if not identity or prefix.startswith(identity):
        return bool(identity)
    for suffix in ("bin", "cli", "daemon", "helper", "renderer", "server"):
        if identity.endswith(suffix) and prefix == identity[: -len(suffix)]:
            return True
    return False


def _extract_version(output: str, argv: tuple[str, ...], args: Mapping[str, Any]) -> str | None:
    identity = _normalise_identity(args.get("version_prefix") or argv[0])
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = _VERSION_TOKEN_RE.search(line)
        if match is None:
            continue
        token = match.group(1)
        if len(token.encode("utf-8", errors="replace")) > MAX_VERSION_TOKEN_BYTES:
            continue
        prefix = re.sub(r"[^a-z0-9]+", "", line[: match.start()].lower())
        if line == match.group(0) or _version_prefix_matches(prefix, identity):
            return token
        if prefix in {"version", "ver", "release", "build"} or prefix.endswith(("version", "release", "build")):
            return token
    return None


def _version_contract(args: Mapping[str, Any]) -> VersionSpec | None:
    expected = args.get("expected_version", args.get("expected", args.get("version")))
    exact = args.get("exact_version")
    if expected is not None and exact is not None and expected != exact:
        return None
    if exact is None:
        exact = expected
    raw_tested = args.get("tested_ranges")
    raw_incompatible = args.get("known_incompatible")
    if raw_tested is not None and (isinstance(raw_tested, (str, bytes)) or not isinstance(raw_tested, Sequence)):
        return None
    if raw_incompatible is not None and (
        isinstance(raw_incompatible, (str, bytes)) or not isinstance(raw_incompatible, Sequence)
    ):
        return None
    fields = {
        "minimum_version": args.get("minimum_version"),
        "maximum_version": args.get("maximum_version"),
        "exact_version": exact,
        "tested_ranges": tuple(raw_tested or ()),
        "known_incompatible": tuple(raw_incompatible or ()),
    }
    if fields["exact_version"] is not None and (
        not isinstance(fields["exact_version"], str) or not fields["exact_version"]
    ):
        return None
    for name in ("minimum_version", "maximum_version"):
        if fields[name] is not None and (
            not isinstance(fields[name], str) or not fields[name]
        ):
            return None
    if any(
        not isinstance(item, str) or not item
        for item in fields["tested_ranges"] + fields["known_incompatible"]
    ):
        return None
    if not any(value for value in fields.values()):
        return VersionSpec()
    return VersionSpec(**fields)


def _resolve_socket_path(raw: object) -> str | None:
    if not isinstance(raw, str) or not raw or "\x00" in raw:
        return None
    if raw == "$XDG_RUNTIME_DIR" or raw.startswith("$XDG_RUNTIME_DIR/"):
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        if not runtime or not runtime.startswith("/"):
            return None
        raw = runtime + raw[len("$XDG_RUNTIME_DIR") :]
    if not raw.startswith("/") or raw.startswith("//"):
        return None
    if any(part in {"", ".", ".."} for part in raw.split("/")[1:]):
        return None
    encoded = os.fsencode(raw)
    if len(encoded) > MAX_SOCKET_PATH_BYTES:
        return None
    return raw


def _socket_endpoint(args: Mapping[str, Any]) -> tuple[SocketEndpoint | None, str | None]:
    endpoint = args.get("endpoint")
    endpoint_args: Mapping[str, Any] = args
    if isinstance(endpoint, Mapping):
        if any(key in args for key in ("kind", "unix_path", "path", "host", "port")):
            return None, "invalid_endpoint"
        endpoint_args = endpoint
    elif endpoint is not None:
        return None, "invalid_endpoint"

    declared_kind = endpoint_args.get("kind")
    if declared_kind is not None and declared_kind not in {"unix", "tcp"}:
        return None, "invalid_endpoint"
    path_value = endpoint_args.get("unix_path", endpoint_args.get("path"))
    if path_value is not None:
        if declared_kind not in {None, "unix"}:
            return None, "invalid_endpoint"
        path = _resolve_socket_path(path_value)
        if path is None:
            return None, "invalid_endpoint"
        if any(key in endpoint_args for key in ("host", "port")):
            return None, "invalid_endpoint"
        return SocketEndpoint("unix", path=path), None

    host = endpoint_args.get("host")
    port = endpoint_args.get("port")
    if declared_kind not in {None, "tcp"}:
        return None, "invalid_endpoint"
    if not isinstance(host, str) or host not in {"localhost", "127.0.0.1", "::1"}:
        if host is not None:
            return None, "remote_endpoint_forbidden"
        return None, "missing_endpoint"
    if type(port) is not int or not 1 <= port <= 65535:
        return None, "invalid_endpoint"
    return SocketEndpoint("tcp", host=host, port=port), None


def _resolve_check_path(
    registry: ManifestRegistry,
    spec: object,
    args: Mapping[str, Any],
) -> tuple[Path | None, object | None, str | None]:
    artifact_id = getattr(spec, "artifact_id", None)
    artifact = None
    if artifact_id is not None:
        if not isinstance(artifact_id, str) or not artifact_id:
            return None, None, "missing_artifact_spec"
        artifact = registry.artifacts.get(artifact_id)
        if artifact is None:
            return None, None, "missing_artifact_spec"
        spec_component_id = getattr(spec, "component_id", None)
        artifact_component_id = getattr(artifact, "component_id", None)
        if (
            isinstance(spec_component_id, str)
            and isinstance(artifact_component_id, str)
            and spec_component_id != artifact_component_id
        ):
            return None, artifact, "artifact_component_mismatch"
        declared = getattr(artifact, "path", None)
    else:
        declared = args.get("path", args.get("file", args.get("config_path")))
    path = resolve_canonical_artifact_path(declared)
    if path is None:
        return None, artifact, "invalid_artifact_path"
    if artifact_id is not None and any(key in args for key in ("path", "file", "config_path")):
        override = resolve_canonical_artifact_path(args.get("path", args.get("file", args.get("config_path"))))
        if override != path:
            return None, artifact, "path_override_forbidden"
    return Path(path), artifact, None


def _validate_observed_type(observation: object, artifact: object | None) -> bool:
    filesystem_type = getattr(observation, "filesystem_type", None)
    if filesystem_type in {None, ""} or artifact is None:
        return True
    expected = _expected_filesystem_type(getattr(artifact, "type", None))
    return expected is None or filesystem_type == expected


def _reap_health_worker(process: Any, cleanup_seconds: float) -> None:
    """Stop and definitively reap a killable health-check process."""

    try:
        if not process.is_alive():
            process.join(timeout=0)
            return
    except (AssertionError, OSError):
        return
    try:
        process.terminate()
    except (OSError, ProcessLookupError):
        pass
    try:
        process.join(timeout=max(0.0, cleanup_seconds))
    except (AssertionError, OSError):
        return
    try:
        still_alive = process.is_alive()
    except (AssertionError, OSError):
        return
    if not still_alive:
        return
    kill = getattr(process, "kill", None)
    if callable(kill):
        try:
            kill()
        except (OSError, ProcessLookupError):
            pass
    try:
        # A killable process boundary is the ownership boundary: do not
        # return while its descriptor-owning worker is still running.
        process.join()
    except (AssertionError, OSError):
        pass


def _run_health_check_in_process(
    executor: "HealthCheckExecutor",
    registry: ManifestRegistry,
    spec: object,
    *,
    timeout_seconds: float,
    deadline: float,
    cancellation_event: Any,
    connection: Any,
) -> None:
    """Run the default read-only check behind a killable POSIX boundary."""

    cancellation = _CancellationToken(deadline, cancellation_event, time.monotonic)
    try:
        payload = executor._run_check(
            registry,
            spec,
            timeout_seconds=timeout_seconds,
            deadline=deadline,
            cancellation=cancellation,
        )
    except BaseException as exc:  # a probe must never kill the Doctor run
        payload = _operation_failure(exc)
    try:
        connection.send(payload)
    except (BrokenPipeError, OSError, ValueError):
        pass
    finally:
        try:
            connection.close()
        except (OSError, ValueError):
            pass


class HealthCheckExecutor:
    """Execute selected canonical checks under per-check and run budgets."""

    def __init__(
        self,
        operations: HealthOperations | None = None,
        *,
        max_checks: int = 128,
        max_seconds: int | float = 30.0,
        max_output_bytes: int = 64 * 1024,
        max_file_bytes: int = MAX_FILE_BYTES,
        worker_cleanup_seconds: int | float = MAX_WORKER_CLEANUP_SECONDS,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.operations: HealthOperations = operations if operations is not None else ReadOnlyHealthOperations()
        self.max_checks = _nonnegative_integer("max_checks", max_checks, MAX_CHECKS)
        self.max_seconds = _finite_nonnegative("max_seconds", max_seconds, MAX_RUN_SECONDS)
        self.max_output_bytes = _nonnegative_integer("max_output_bytes", max_output_bytes, MAX_OUTPUT_BYTES)
        self.max_file_bytes = _nonnegative_integer("max_file_bytes", max_file_bytes, MAX_FILE_BYTES)
        self.worker_cleanup_seconds = _finite_nonnegative(
            "worker_cleanup_seconds", worker_cleanup_seconds, MAX_WORKER_CLEANUP_SECONDS
        )
        self.clock = clock

    def execute(
        self,
        registry: ManifestRegistry,
        *,
        context: str = "doctor_background",
        max_cost: str = "cheap",
        allowed_side_effects: Sequence[str] = ("none", "read_only"),
        check_ids: Sequence[str] | None = None,
    ) -> HealthCheckReport:
        """Run canonical policy-selected checks, optionally filtered by IDs.

        ``check_ids`` never bypasses ``select_health_checks``.  An explicitly
        requested ID outside the policy is returned as ``NOT_APPLICABLE`` and
        does not cause an operation to run.
        """

        policy_ids = tuple(
            select_health_checks(
                registry,
                context=context,
                max_cost=max_cost,
                allowed_side_effects=allowed_side_effects,
            )
        )
        if check_ids is None:
            requested_ids = policy_ids
        else:
            if isinstance(check_ids, (str, bytes)) or not isinstance(check_ids, Sequence):
                raise ValueError("check_ids must be a sequence of strings")
            requested_ids = tuple(check_ids)
            if any(type(check_id) is not str or not check_id for check_id in requested_ids):
                raise ValueError("check_ids must contain non-empty strings")

        started = self.clock()
        deadline = started + self.max_seconds
        results: list[HealthCheckResult] = []
        cache: dict[str, HealthCheckResult] = {}
        attempted = 0
        cache_hits = 0
        budget_exhausted = False

        for check_id in requested_ids:
            spec = registry.health_checks.get(check_id)
            if spec is None:
                result = HealthCheckResult(
                    check_id,
                    None,
                    None,
                    HealthStatus.UNKNOWN,
                    "missing_check_spec",
                    detail="canonical health-check specification is unavailable",
                )
                results.append(result)
                continue

            component_id = getattr(spec, "component_id", None)
            check_kind = getattr(spec, "check", None)
            args = getattr(spec, "args", None)
            key = _cache_key(check_id, args)
            if not isinstance(check_kind, str) or check_kind not in SUPPORTED_CHECK_TYPES:
                result = HealthCheckResult(
                    check_id,
                    component_id,
                    check_kind if isinstance(check_kind, str) else None,
                    HealthStatus.UNKNOWN,
                    "unsupported_check_type",
                    detail="health-check type is not supported by this Doctor executor",
                )
                results.append(result)
                cache[key] = result
                continue
            if key in cache:
                cached = replace(cache[key], cached=True)
                results.append(cached)
                cache_hits += 1
                continue

            if check_id not in policy_ids:
                result = HealthCheckResult(
                    check_id,
                    component_id,
                    check_kind if isinstance(check_kind, str) else None,
                    HealthStatus.NOT_APPLICABLE,
                    "not_selected_by_policy",
                    detail="check is outside the canonical context, cost, or side-effect policy",
                )
                results.append(result)
                cache[key] = result
                continue

            now = self.clock()
            if attempted >= self.max_checks or now >= deadline:
                budget_exhausted = True
                result = HealthCheckResult(
                    check_id,
                    component_id,
                    check_kind if isinstance(check_kind, str) else None,
                    HealthStatus.UNKNOWN,
                    "budget_exhausted",
                    detail="Doctor run budget was exhausted before this check",
                )
                results.append(result)
                continue

            timeout_seconds = self._check_timeout(spec, deadline - now)
            if timeout_seconds is None:
                result = HealthCheckResult(
                    check_id,
                    component_id,
                    check_kind if isinstance(check_kind, str) else None,
                    HealthStatus.UNKNOWN,
                    "invalid_timeout",
                    detail="health-check timeout is not a bounded positive number",
                )
                results.append(result)
                continue
            if timeout_seconds <= 0:
                budget_exhausted = True
                result = HealthCheckResult(
                    check_id,
                    component_id,
                    check_kind if isinstance(check_kind, str) else None,
                    HealthStatus.UNKNOWN,
                    "budget_exhausted",
                    detail="Doctor run budget was exhausted before this check",
                )
                results.append(result)
                continue

            attempted += 1
            payload, duration_ms = self._run_with_worker(
                registry,
                spec,
                timeout_seconds=timeout_seconds,
                deadline=deadline,
            )
            if self.clock() >= deadline:
                budget_exhausted = True
                if payload.status is not HealthStatus.UNKNOWN:
                    payload = _payload(
                        HealthStatus.UNKNOWN,
                        "budget_exhausted",
                        detail="Doctor run budget expired before the observation completed",
                        stdout=payload.stdout,
                        stderr=payload.stderr,
                        output_truncated=payload.output_truncated,
                    )
            elif payload.reason_code == "budget_exhausted":
                budget_exhausted = True
            result = HealthCheckResult(
                check_id,
                component_id,
                check_kind if isinstance(check_kind, str) else None,
                payload.status,
                payload.reason_code,
                detail=payload.detail,
                duration_ms=max(0.0, duration_ms),
                stdout=payload.stdout,
                stderr=payload.stderr,
                value=payload.value,
                output_truncated=payload.output_truncated,
            )
            results.append(result)
            cache[key] = result

        return HealthCheckReport(
            tuple(results),
            max(0.0, (self.clock() - started) * 1000.0),
            budget_exhausted=budget_exhausted,
            check_ids=tuple(requested_ids),
            policy_check_ids=policy_ids,
            cache_hits=cache_hits,
        )

    run = execute

    @staticmethod
    def _call_operation(method: Callable[..., object], *args: object, **kwargs: object) -> object:
        """Call an injected seam without forcing unused optional keywords.

        This keeps the seam pleasant for tiny test doubles while the default
        implementation still receives every bound limit.
        """

        try:
            signature = inspect.signature(method)
        except (TypeError, ValueError):
            return method(*args, **kwargs)
        if any(parameter.kind is inspect.Parameter.VAR_KEYWORD for parameter in signature.parameters.values()):
            return method(*args, **kwargs)
        accepted = {
            name: value
            for name, value in kwargs.items()
            if name in signature.parameters
            and signature.parameters[name].kind
            in {inspect.Parameter.POSITIONAL_OR_KEYWORD, inspect.Parameter.KEYWORD_ONLY}
        }
        return method(*args, **accepted)

    def _check_timeout(self, spec: object, remaining: float) -> float | None:
        raw = getattr(spec, "timeout_ms", None)
        if type(raw) is not int or raw <= 0:
            return None
        seconds = raw / 1000.0
        if not math.isfinite(seconds) or seconds <= 0 or seconds > MAX_CHECK_SECONDS:
            return None
        if remaining <= 0:
            return 0.0
        return min(seconds, remaining)

    def _run_with_process(
        self,
        registry: ManifestRegistry,
        spec: object,
        *,
        timeout_seconds: float,
        deadline: float,
    ) -> tuple[_CheckPayload, float]:
        """Run default operations in a killable process with one deadline."""

        started = self.clock()
        try:
            context = multiprocessing.get_context("fork")
        except (AttributeError, ValueError):
            return (
                _payload(
                    HealthStatus.UNKNOWN,
                    "worker_unavailable",
                    detail="default health operations require a fork-capable worker",
                ),
                max(0.0, (self.clock() - started) * 1000.0),
            )

        parent_connection, child_connection = context.Pipe(duplex=False)
        cancellation_event = context.Event()
        process: Any = None
        process_started = False
        try:
            process = context.Process(
                target=_run_health_check_in_process,
                args=(self, registry, spec),
                kwargs={
                    "timeout_seconds": timeout_seconds,
                    "deadline": deadline,
                    "cancellation_event": cancellation_event,
                    "connection": child_connection,
                },
            )
            process.daemon = False
            process.start()
            process_started = True
            child_connection.close()

            remaining = max(0.0, deadline - self.clock())
            raw: object | None = None
            if remaining > 0 and parent_connection.poll(remaining):
                try:
                    raw = parent_connection.recv()
                except (EOFError, OSError, ValueError):
                    raw = None
            if isinstance(raw, _CheckPayload):
                _reap_health_worker(process, self.worker_cleanup_seconds)
                return raw, max(0.0, (self.clock() - started) * 1000.0)

            expired = self.clock() >= deadline
            cancellation_event.set()
            _reap_health_worker(process, self.worker_cleanup_seconds)
            code = "budget_exhausted" if expired else "operation_error"
            return (
                _payload(
                    HealthStatus.UNKNOWN,
                    code,
                    detail="bounded default health worker returned no result",
                ),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        except (OSError, RuntimeError, ValueError) as exc:
            cancellation_event.set()
            if process_started:
                _reap_health_worker(process, self.worker_cleanup_seconds)
            return (
                _payload(
                    HealthStatus.UNKNOWN,
                    "operation_error",
                    detail=f"default health worker failed: {type(exc).__name__}",
                ),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        finally:
            try:
                child_connection.close()
            except (OSError, ValueError):
                pass
            try:
                parent_connection.close()
            except (OSError, ValueError):
                pass
            if process_started:
                _reap_health_worker(process, self.worker_cleanup_seconds)

    def _run_with_worker(
        self,
        registry: ManifestRegistry,
        spec: object,
        *,
        timeout_seconds: float,
        deadline: float,
    ) -> tuple[_CheckPayload, float]:
        if isinstance(self.operations, ReadOnlyHealthOperations):
            return self._run_with_process(
                registry,
                spec,
                timeout_seconds=timeout_seconds,
                deadline=deadline,
            )

        started = self.clock()
        holder: list[_CheckPayload] = []
        cancellation = _CancellationToken(deadline, clock=self.clock)

        def worker() -> None:
            try:
                holder.append(
                    self._run_check(
                        registry,
                        spec,
                        timeout_seconds=timeout_seconds,
                        deadline=deadline,
                        cancellation=cancellation,
                    )
                )
            except BaseException as exc:  # a probe must never kill the Doctor run
                holder.append(_operation_failure(exc))

        thread = threading.Thread(target=worker, name="realmheart-doctor-probe", daemon=True)
        thread.start()
        thread.join(min(timeout_seconds, max(0.0, deadline - self.clock())))
        if thread.is_alive():
            expired = self.clock() >= deadline
            cancellation.cancel()
            thread.join(self.worker_cleanup_seconds)
            code = "budget_exhausted" if expired else "timeout"
            return (
                _payload(
                    HealthStatus.UNKNOWN,
                    code,
                    detail="bounded probe worker did not finish before its deadline",
                ),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        if not holder:
            return (
                _payload(HealthStatus.UNKNOWN, "operation_error", detail="probe worker returned no result"),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        return holder[0], max(0.0, (self.clock() - started) * 1000.0)

    def _run_check(
        self,
        registry: ManifestRegistry,
        spec: object,
        *,
        timeout_seconds: float,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> _CheckPayload:
        check_kind = getattr(spec, "check", None)
        args: object = getattr(spec, "args", None)
        if not isinstance(check_kind, str) or check_kind not in SUPPORTED_CHECK_TYPES:
            return _payload(
                HealthStatus.UNKNOWN,
                "unsupported_check_type",
                detail="health-check type is not supported by this Doctor executor",
            )
        if not isinstance(args, Mapping):
            if check_kind in {"version_probe", "runtime_probe", "process_start_smoke"} and isinstance(
                args, Sequence
            ) and not isinstance(args, (str, bytes)):
                return self._command_check(
                    registry,
                    spec,
                    args,
                    timeout_seconds,
                    version=check_kind == "version_probe",
                    deadline=deadline,
                    cancellation=cancellation,
                )
            return _payload(HealthStatus.UNKNOWN, "invalid_check_spec", detail="health-check arguments are not an object")

        if check_kind == "artifact_exists":
            return self._artifact_exists(registry, spec, args, timeout_seconds)
        if check_kind == "artifact_executable":
            return self._artifact_executable(registry, spec, args, timeout_seconds)
        if check_kind == "file_hash_matches":
            return self._file_hash_matches(registry, spec, args, timeout_seconds)
        if check_kind == "config_parse":
            return self._config_parse(registry, spec, args, timeout_seconds)
        if check_kind == "socket_reachable":
            return self._socket_reachable(args, timeout_seconds)
        if check_kind == "version_probe":
            return self._command_check(
                registry,
                spec,
                args,
                timeout_seconds,
                version=True,
                deadline=deadline,
                cancellation=cancellation,
            )
        if check_kind == "runtime_probe":
            return self._command_check(
                registry,
                spec,
                args,
                timeout_seconds,
                version=False,
                deadline=deadline,
                cancellation=cancellation,
            )
        if check_kind == "process_start_smoke":
            return self._command_check(
                registry,
                spec,
                args,
                timeout_seconds,
                version=False,
                deadline=deadline,
                cancellation=cancellation,
            )
        return _payload(HealthStatus.UNKNOWN, "unsupported_check_type")

    def _observe_path(
        self,
        path: Path,
        *,
        include_sha256: bool,
        timeout_seconds: float,
    ) -> PathObservation | None:
        raw = self._call_operation(
            self.operations.observe_path,
            path,
            include_sha256=include_sha256,
            max_bytes=self.max_file_bytes,
            max_seconds=min(timeout_seconds, MAX_CHECK_SECONDS),
        )
        return _coerce_path_observation(raw)

    def _artifact_exists(
        self,
        registry: ManifestRegistry,
        spec: object,
        args: Mapping[str, Any],
        timeout_seconds: float,
    ) -> _CheckPayload:
        path, artifact, error = _resolve_check_path(registry, spec, args)
        if error:
            return _payload(HealthStatus.UNKNOWN, error, detail="canonical artifact path could not be established")
        if path is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_artifact_path", detail="canonical artifact path could not be established")
        if artifact is not None and _expected_filesystem_type(getattr(artifact, "type", None)) is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_artifact_spec", detail="canonical artifact type is unsupported")
        try:
            observation = self._observe_path(path, include_sha256=False, timeout_seconds=timeout_seconds)
        except BaseException as exc:
            return _operation_failure(exc, default_code="observation_unavailable")
        if observation is None:
            return _payload(HealthStatus.UNKNOWN, "observation_unavailable", detail="path operation returned no valid observation")
        if not getattr(observation, "exists", False):
            return _payload(HealthStatus.FAIL, "artifact_missing", detail="canonical artifact is absent")
        if not _validate_observed_type(observation, artifact):
            return _payload(HealthStatus.FAIL, "artifact_type_mismatch", detail="canonical artifact has the wrong filesystem type")
        if getattr(observation, "filesystem_type", None) == "symlink":
            return _payload(HealthStatus.FAIL, "artifact_type_mismatch", detail="canonical artifact must not be a symlink")
        return _payload(HealthStatus.PASS, "observed", detail="canonical artifact exists")

    def _artifact_executable(
        self,
        registry: ManifestRegistry,
        spec: object,
        args: Mapping[str, Any],
        timeout_seconds: float,
    ) -> _CheckPayload:
        path, artifact, error = _resolve_check_path(registry, spec, args)
        if error:
            return _payload(HealthStatus.UNKNOWN, error, detail="canonical artifact path could not be established")
        if path is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_artifact_path", detail="canonical artifact path could not be established")
        try:
            observation = self._observe_path(path, include_sha256=False, timeout_seconds=timeout_seconds)
        except BaseException as exc:
            return _operation_failure(exc, default_code="observation_unavailable")
        if observation is None:
            return _payload(HealthStatus.UNKNOWN, "observation_unavailable", detail="path operation returned no valid observation")
        if not getattr(observation, "exists", False):
            return _payload(HealthStatus.FAIL, "artifact_missing", detail="canonical executable is absent")
        filesystem_type = getattr(observation, "filesystem_type", None)
        expected_type = _expected_filesystem_type(getattr(artifact, "type", None))
        if (
            (artifact is not None and expected_type != "file")
            or not _validate_observed_type(observation, artifact)
            or filesystem_type not in {None, "file"}
        ):
            return _payload(HealthStatus.FAIL, "artifact_type_mismatch", detail="executable artifact is not a regular file")
        mode = _mode_value(getattr(observation, "mode", None))
        if mode is None:
            return _payload(HealthStatus.UNKNOWN, "mode_unavailable", detail="executable mode could not be observed")
        if not mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            return _payload(HealthStatus.FAIL, "artifact_not_executable", detail="executable artifact has no execute bit")
        if not mode & (stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH):
            return _payload(
                HealthStatus.FAIL,
                "artifact_not_readable",
                detail="executable artifact must be readable for format authorization",
            )
        if mode & (stat.S_ISUID | stat.S_ISGID | stat.S_ISVTX):
            return _payload(
                HealthStatus.FAIL,
                "special_mode_forbidden",
                detail="executable artifact has a special permission bit",
            )
        return _payload(HealthStatus.PASS, "observed", detail="canonical artifact is executable")

    def _file_hash_matches(
        self,
        registry: ManifestRegistry,
        spec: object,
        args: Mapping[str, Any],
        timeout_seconds: float,
    ) -> _CheckPayload:
        expected = args.get(
            "expected_sha256",
            args.get("expected_hash", args.get("sha256", args.get("hash"))),
        )
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expected):
            return _payload(HealthStatus.UNKNOWN, "invalid_expected_hash", detail="a SHA-256 digest is required")
        path, artifact, error = _resolve_check_path(registry, spec, args)
        if error:
            return _payload(HealthStatus.UNKNOWN, error, detail="canonical artifact path could not be established")
        if path is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_artifact_path", detail="canonical artifact path could not be established")
        if artifact is not None:
            expected_type = _expected_filesystem_type(getattr(artifact, "type", None))
            if expected_type is None:
                return _payload(HealthStatus.UNKNOWN, "invalid_artifact_spec", detail="canonical artifact type is unsupported")
            if expected_type != "file":
                return _payload(
                    HealthStatus.NOT_APPLICABLE,
                    "hash_requires_regular_file",
                    detail="file hash checks apply only to regular-file artifacts",
                )
        try:
            observation = self._observe_path(path, include_sha256=True, timeout_seconds=timeout_seconds)
            if observation is None:
                return _payload(HealthStatus.UNKNOWN, "observation_unavailable", detail="path operation returned no valid observation")
            if not getattr(observation, "exists", False):
                return _payload(HealthStatus.FAIL, "artifact_missing", detail="hashed artifact is absent")
            if getattr(observation, "filesystem_type", None) not in {None, "file"}:
                return _payload(HealthStatus.UNKNOWN, "hash_unavailable", detail="hashed artifact is not a regular file")
            actual = getattr(observation, "sha256", None)
            if actual is not None and (
                not isinstance(actual, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", actual)
            ):
                return _payload(HealthStatus.UNKNOWN, "hash_unavailable", detail="file operation returned an invalid digest")
            if actual is None:
                content = self._call_operation(
                    self.operations.read_regular_file,
                    path,
                    max_bytes=self.max_file_bytes,
                )
                if not isinstance(content, bytes):
                    return _payload(
                        HealthStatus.UNKNOWN,
                        "hash_unavailable",
                        detail="file operation returned no byte content",
                    )
                if len(content) > self.max_file_bytes:
                    return _payload(HealthStatus.UNKNOWN, "observation_limit_exceeded", detail="hashed file exceeds the byte limit")
                actual = hashlib.sha256(content).hexdigest()
        except BaseException as exc:
            return _operation_failure(exc, default_code="hash_unavailable")
        if actual.lower() != expected.lower():
            return _payload(HealthStatus.FAIL, "hash_mismatch", detail="observed SHA-256 differs from the declared digest")
        return _payload(HealthStatus.PASS, "observed", detail="declared SHA-256 matches")

    def _config_parse(
        self,
        registry: ManifestRegistry,
        spec: object,
        args: Mapping[str, Any],
        timeout_seconds: float,
    ) -> _CheckPayload:
        path, artifact, error = _resolve_check_path(registry, spec, args)
        if error:
            return _payload(HealthStatus.UNKNOWN, error, detail="canonical config path could not be established")
        if path is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_artifact_path", detail="canonical config path could not be established")
        if artifact is not None:
            expected_type = _expected_filesystem_type(getattr(artifact, "type", None))
            if expected_type is None:
                return _payload(HealthStatus.UNKNOWN, "invalid_artifact_spec", detail="canonical artifact type is unsupported")
            if expected_type != "file":
                return _payload(
                    HealthStatus.NOT_APPLICABLE,
                    "config_requires_regular_file",
                    detail="config parsing applies only to regular-file artifacts",
                )
        try:
            observation = self._observe_path(path, include_sha256=False, timeout_seconds=timeout_seconds)
            if observation is None:
                return _payload(HealthStatus.UNKNOWN, "observation_unavailable", detail="path operation returned no valid observation")
            if not getattr(observation, "exists", False):
                return _payload(HealthStatus.FAIL, "artifact_missing", detail="config artifact is absent")
            if getattr(observation, "filesystem_type", None) not in {None, "file"}:
                return _payload(HealthStatus.UNKNOWN, "config_unavailable", detail="config artifact is not a regular file")
            content = self._call_operation(
                self.operations.read_regular_file,
                path,
                max_bytes=self.max_file_bytes,
            )
            if not isinstance(content, bytes):
                return _payload(
                    HealthStatus.UNKNOWN,
                    "config_unavailable",
                    detail="file operation returned no byte content",
                )
            if len(content) > self.max_file_bytes:
                return _payload(HealthStatus.UNKNOWN, "observation_limit_exceeded", detail="config exceeds the byte limit")
        except BaseException as exc:
            return _operation_failure(exc, default_code="config_unavailable")

        format_name = args.get("format", args.get("parser"))
        if format_name is None:
            format_name = Path(path).suffix.lower().lstrip(".") or "json"
        if not isinstance(format_name, str):
            return _payload(HealthStatus.UNKNOWN, "unsupported_format", detail="config parser name is invalid")
        format_name = format_name.lower()
        try:
            text = content.decode("utf-8")
            if format_name == "json":
                json.loads(text)
            elif format_name in {"toml", "tml"}:
                tomllib.loads(text)
            elif format_name in {"ini", "cfg", "configparser"}:
                parser = configparser.ConfigParser(interpolation=None)
                parser.read_string(text)
            else:
                return _payload(HealthStatus.UNKNOWN, "unsupported_format", detail="config parser is not supported")
        except (UnicodeError, json.JSONDecodeError, tomllib.TOMLDecodeError, configparser.Error, RecursionError):
            return _payload(HealthStatus.FAIL, "config_malformed", detail="config content could not be parsed")
        return _payload(HealthStatus.PASS, "observed", detail="config parsed successfully")

    def _socket_reachable(self, args: Mapping[str, Any], timeout_seconds: float) -> _CheckPayload:
        endpoint, error = _socket_endpoint(args)
        if error:
            return _payload(HealthStatus.UNKNOWN, error, detail="socket endpoint is not an explicitly declared local endpoint")
        if endpoint is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_endpoint", detail="socket endpoint is not an explicitly declared local endpoint")
        try:
            raw = self._call_operation(
                self.operations.socket_reachable,
                endpoint,
                timeout=min(timeout_seconds, MAX_SOCKET_SECONDS),
            )
            observation = _coerce_socket_observation(raw)
        except BaseException as exc:
            return _operation_failure(exc, default_code="socket_unavailable")
        if observation is None:
            return _payload(HealthStatus.UNKNOWN, "socket_unavailable", detail="socket operation returned no observation")
        code = _normalise_error_code(observation.error_code)
        if observation.reachable:
            return _payload(HealthStatus.PASS, "observed", detail="local socket accepted a connection")
        if code in {"timeout", "permission_denied", "socket_error", "socket_unavailable"}:
            return _payload(
                HealthStatus.UNKNOWN,
                code or "socket_unavailable",
                detail="local socket reachability was not established",
            )
        if code in {"remote_endpoint_forbidden", "invalid_endpoint", "missing_endpoint"}:
            return _payload(HealthStatus.UNKNOWN, code, detail="socket endpoint is not valid for Doctor")
        if code in {None, "connection_refused", "endpoint_missing", "socket_unreachable"}:
            return _payload(
                HealthStatus.FAIL,
                code or "socket_unreachable",
                detail="local socket did not accept a connection",
            )
        return _payload(HealthStatus.UNKNOWN, code, detail="local socket reachability was not established")

    def _command_check(
        self,
        registry: ManifestRegistry,
        spec: object,
        args: Mapping[str, Any] | Sequence[object],
        timeout_seconds: float,
        *,
        version: bool,
        deadline: float | None = None,
        cancellation: _CancellationToken | None = None,
    ) -> _CheckPayload:
        command_args: Mapping[str, Any]
        if isinstance(args, Mapping):
            command_args = args
        elif isinstance(args, Sequence) and not isinstance(args, (str, bytes)):
            command_args = {"args": tuple(args)}
        else:
            return _payload(HealthStatus.UNKNOWN, "invalid_check_spec", detail="health-check arguments are not command data")

        artifact_executable: str | None = None
        if getattr(spec, "artifact_id", None) is not None:
            path, artifact, error = _resolve_check_path(registry, spec, command_args)
            if error:
                return _payload(HealthStatus.UNKNOWN, error, detail="canonical executable path could not be established")
            if path is None or artifact is None:
                return _payload(HealthStatus.UNKNOWN, "missing_artifact_spec", detail="canonical executable path could not be established")
            if getattr(artifact, "type", None) != "executable":
                return _payload(
                    HealthStatus.UNKNOWN,
                    "artifact_type_mismatch",
                    detail="command probe artifact must be declared as executable",
                )
            artifact_executable = os.fspath(path)

        argv = _command_from_args(
            command_args,
            version=version,
            artifact_executable=artifact_executable,
        )
        if argv is None:
            return _payload(HealthStatus.UNKNOWN, "invalid_command", detail="Doctor accepts only a non-empty structured argv")
        if artifact_executable is None and not _trusted_absolute_executable(argv[0]):
            return _payload(
                HealthStatus.UNKNOWN,
                "invalid_command",
                detail="artifact-less command probes require a trusted absolute executable",
            )
        if not _authorise_command_argv(
            argv,
            artifact_bound=artifact_executable is not None,
            version=version,
        ):
            return _payload(
                HealthStatus.UNKNOWN,
                "invalid_command",
                detail="executable identity or argument schema is not approved",
            )
        try:
            if artifact_executable is not None:
                raw = self._call_operation(
                    self.operations.run_descriptor,
                    Path(artifact_executable),
                    argv,
                    timeout=timeout_seconds,
                    max_output_bytes=self.max_output_bytes,
                    deadline=deadline,
                    cancellation=cancellation,
                )
            else:
                raw = self._call_operation(
                    self.operations.run,
                    argv,
                    timeout=timeout_seconds,
                    max_output_bytes=self.max_output_bytes,
                    deadline=deadline,
                    cancellation=cancellation,
                )
            observation = _coerce_command_observation(raw, argv)
        except BaseException as exc:
            return _operation_failure(exc)
        if observation is None:
            reason = "operation_error" if raw is None else "malformed_output"
            return _payload(HealthStatus.UNKNOWN, reason, detail="process operation returned no valid observation")
        stdout, stderr, bounded = _bounded_outputs(
            observation.stdout,
            observation.stderr,
            self.max_output_bytes,
        )
        output_truncated = bounded or observation.output_limited
        if output_truncated:
            return _payload(
                HealthStatus.UNKNOWN,
                "output_limit_exceeded",
                detail="captured process output exceeded the byte limit",
                stdout=stdout,
                stderr=stderr,
                output_truncated=True,
            )
        code = _normalise_error_code(observation.error_code)
        if observation.timed_out or code == "timeout":
            return _payload(
                HealthStatus.UNKNOWN,
                "timeout",
                detail="process exceeded its observation deadline",
                stdout=stdout,
                stderr=stderr,
            )
        if code in {"executable_missing", "missing_executable"}:
            return _payload(
                HealthStatus.UNKNOWN,
                "executable_missing",
                detail="process executable could not be found",
                stdout=stdout,
                stderr=stderr,
            )
        if code == "permission_denied":
            return _payload(
                HealthStatus.UNKNOWN,
                "permission_denied",
                detail="process execution was not permitted",
                stdout=stdout,
                stderr=stderr,
            )
        if code is not None:
            return _payload(
                HealthStatus.UNKNOWN,
                code,
                detail="process observation failed before a result was established",
                stdout=stdout,
                stderr=stderr,
            )
        if observation.returncode is None:
            return _payload(
                HealthStatus.UNKNOWN,
                "operation_error",
                detail="process returned no exit status",
                stdout=stdout,
                stderr=stderr,
            )
        if observation.returncode != 0:
            return _payload(
                HealthStatus.FAIL,
                "command_failed",
                detail="process returned a non-zero exit status",
                stdout=stdout,
                stderr=stderr,
            )
        if not version:
            return _payload(HealthStatus.PASS, "observed", stdout=stdout, stderr=stderr)

        detected = _extract_version(stdout + ("\n" if stdout and stderr else "") + stderr, argv, command_args)
        try:
            parsed_version = ParsedVersion.parse(detected)
        except (TypeError, ValueError, OverflowError):
            parsed_version = None
        if detected is None or parsed_version is None:
            return _payload(
                HealthStatus.UNKNOWN,
                "malformed_output",
                detail="version probe returned no identifiable version evidence",
                stdout=stdout,
                stderr=stderr,
            )
        contract = _version_contract(command_args)
        if contract is None:
            return _payload(
                HealthStatus.UNKNOWN,
                "invalid_version_contract",
                detail="version compatibility arguments are malformed",
                stdout=stdout,
                stderr=stderr,
                value=detected,
            )
        try:
            compatibility = classify_version(contract, detected)
        except (TypeError, ValueError):
            return _payload(
                HealthStatus.UNKNOWN,
                "invalid_version_contract",
                detail="version compatibility arguments are invalid",
                stdout=stdout,
                stderr=stderr,
                value=detected,
            )
        if compatibility.value == "incompatible":
            return _payload(
                HealthStatus.FAIL,
                "version_mismatch",
                detail="observed version violates the declared compatibility contract",
                stdout=stdout,
                stderr=stderr,
                value=detected,
            )
        return _payload(
            HealthStatus.PASS,
            "observed",
            stdout=stdout,
            stderr=stderr,
            value=detected,
        )


def execute_health_checks(
    registry: ManifestRegistry,
    *,
    operations: HealthOperations | None = None,
    context: str = "doctor_background",
    max_cost: str = "cheap",
    allowed_side_effects: Sequence[str] = ("none", "read_only"),
    check_ids: Sequence[str] | None = None,
    max_checks: int = 128,
    max_seconds: int | float = 30.0,
    max_output_bytes: int = 64 * 1024,
    max_file_bytes: int = MAX_FILE_BYTES,
) -> HealthCheckReport:
    """Convenience wrapper for one run-scoped executor invocation."""

    return HealthCheckExecutor(
        operations,
        max_checks=max_checks,
        max_seconds=max_seconds,
        max_output_bytes=max_output_bytes,
        max_file_bytes=max_file_bytes,
    ).execute(
        registry,
        context=context,
        max_cost=max_cost,
        allowed_side_effects=allowed_side_effects,
        check_ids=check_ids,
    )


run_health_checks = execute_health_checks
DoctorHealthExecutor = HealthCheckExecutor

__all__ = [
    "CommandObservation",
    "DefaultHealthOperations",
    "DoctorHealthExecutor",
    "HealthCheckExecutionReport",
    "HealthCheckExecutionResult",
    "HealthCheckExecutor",
    "HealthCheckOutcome",
    "HealthCheckReport",
    "HealthCheckResult",
    "HealthCheckRun",
    "HealthCheckState",
    "HealthCheckStatus",
    "HealthOperations",
    "HealthStatus",
    "ReadOnlyHealthOperations",
    "SocketEndpoint",
    "SocketObservation",
    "MAX_SOCKET_SECONDS",
    "SUPPORTED_CHECK_TYPES",
    "execute_health_checks",
    "run_health_checks",
]
