"""Bounded, read-only Doctor health-check execution.

The manifest remains the only source of check identity and policy.  This module
only translates canonical ``HealthCheckSpec`` records into bounded observations;
it never repairs, activates, or mutates Realmheart state.
"""
from __future__ import annotations

import configparser
import ctypes
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
# Set only inside the forked Doctor worker after it owns a private process
# session.  A probe in that session must not kill the worker when it cleans up
# its own child; the request-side reaper owns group-wide termination.
_HEALTH_WORKER_GROUP_OWNED = False
_HEALTH_WORKER_IDENTITY: _HealthWorkerIdentity | None = None
_HEALTH_WORKER_TRACKED_DESCENDANTS: dict[int, int | None] = {}
_HEALTH_WORKER_REAPED_PROBES: set[tuple[int, int]] = set()
_HEALTH_WORKER_CONTAINMENT_VALID = True
_HEALTH_PR_SET_PDEATHSIG = 1
_HEALTH_PR_SET_CHILD_SUBREAPER = 36
_HEALTH_PR_GET_CHILD_SUBREAPER = 37
_HEALTH_PIDFD_SEND_SIGNAL = 424
_HEALTH_PIDFD_OPEN = 434
_HEALTH_PROC_SCAN_LIMIT = 65536
_REASON_CODE_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SECRET_REPLACEMENT = "[REDACTED]"


@dataclass(frozen=True)
class _HealthWorkerIdentity:
    """Verified identity of the private session leader used by one worker."""

    pid: int
    session_id: int
    process_group_id: int
    start_time: int | None = None
    child_subreaper: bool = False
    verified: bool = False


@dataclass(frozen=True)
class _HealthProcessRecord:
    """Small immutable /proc record used for descendant identity tracking."""

    pid: int
    parent_pid: int
    process_group_id: int
    session_id: int
    start_time: int
    state: str


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


def _health_prctl(option: int, argument: int) -> bool:
    """Call Linux ``prctl`` without making it a required import dependency."""

    if os.name != "posix":
        return False
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        prctl = getattr(libc, "prctl", None)
        if prctl is None:
            return False
        prctl.argtypes = [
            ctypes.c_int,
            ctypes.c_ulong,
            ctypes.c_ulong,
            ctypes.c_ulong,
            ctypes.c_ulong,
        ]
        prctl.restype = ctypes.c_int
        return int(prctl(int(option), ctypes.c_ulong(argument), 0, 0, 0)) == 0
    except (AttributeError, NotImplementedError, OSError, TypeError, ValueError):
        return False


def _health_child_subreaper_state() -> bool | None:
    """Read the current child-subreaper bit without changing it."""

    if os.name != "posix":
        return None
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        prctl = getattr(libc, "prctl", None)
        if prctl is None:
            return None
        value = ctypes.c_int()
        prctl.argtypes = [
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_ulong,
            ctypes.c_ulong,
            ctypes.c_ulong,
        ]
        prctl.restype = ctypes.c_int
        result = int(
            prctl(
                _HEALTH_PR_GET_CHILD_SUBREAPER,
                ctypes.byref(value),
                0,
                0,
                0,
            )
        )
        return bool(value.value) if result == 0 else None
    except (AttributeError, NotImplementedError, OSError, TypeError, ValueError):
        return None


def _set_health_child_subreaper(enabled: bool) -> bool:
    """Set the request-side subreaper bit for one bounded cleanup window."""

    return _health_prctl(_HEALTH_PR_SET_CHILD_SUBREAPER, 1 if enabled else 0)


def _enable_health_child_subreaper() -> bool:
    """Make orphaned probe descendants reparent to their Doctor supervisor."""

    return _set_health_child_subreaper(True)


def _health_probe_preexec() -> None:
    """Give a direct probe a kernel-enforced worker-death backstop."""

    if not _health_prctl(_HEALTH_PR_SET_PDEATHSIG, signal.SIGKILL):
        raise OSError(errno.ENOSYS, "probe parent-death containment is unavailable")


def _health_process_boundary_owned() -> bool:
    """Return whether this process is inside the Doctor-owned boundary."""

    return bool(
        _HEALTH_WORKER_GROUP_OWNED
        and _HEALTH_WORKER_IDENTITY is not None
        and _HEALTH_WORKER_IDENTITY.verified
        and _HEALTH_WORKER_CONTAINMENT_VALID
    )


def _health_pidfd_open(pid: int) -> int | None:
    """Open an identity-bound process handle without a raw-PID fallback."""

    if os.name != "posix" or type(pid) is not int or pid <= 0:
        return None
    opener = getattr(os, "pidfd_open", None)
    if callable(opener):
        try:
            descriptor = opener(pid, 0)
            return descriptor if type(descriptor) is int and descriptor >= 0 else None
        except (AttributeError, NotImplementedError, OSError, ProcessLookupError, TypeError, ValueError):
            return None
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        syscall = getattr(libc, "syscall", None)
        if syscall is None:
            return None
        syscall.restype = ctypes.c_long
        descriptor = int(syscall(_HEALTH_PIDFD_OPEN, ctypes.c_int(pid), ctypes.c_uint(0)))
        if descriptor < 0:
            return None
        return descriptor
    except (AttributeError, NotImplementedError, OSError, TypeError, ValueError):
        return None


def _health_pidfd_send_signal(descriptor: int, signal_number: int) -> bool:
    """Send one signal through a pidfd, never through a PID."""

    if type(descriptor) is not int or descriptor < 0 or not isinstance(signal_number, int):
        return False
    sender = getattr(signal, "pidfd_send_signal", None)
    if callable(sender):
        try:
            sender(descriptor, signal_number, None, 0)
            return True
        except (AttributeError, NotImplementedError, OSError, ProcessLookupError, TypeError, ValueError):
            return False
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        syscall = getattr(libc, "syscall", None)
        if syscall is None:
            return False
        syscall.restype = ctypes.c_long
        result = int(
            syscall(
                _HEALTH_PIDFD_SEND_SIGNAL,
                ctypes.c_int(descriptor),
                ctypes.c_int(signal_number),
                ctypes.c_void_p(0),
                ctypes.c_uint(0),
            )
        )
        return result == 0
    except (AttributeError, NotImplementedError, OSError, TypeError, ValueError):
        return False


def _health_signal_verified_private_group(
    record: _HealthProcessRecord,
    expected_start: int,
    signal_number: int,
    private_boundary: _HealthWorkerIdentity | None = None,
) -> bool:
    """Use only a verified private process group when pidfds are unavailable."""

    if record.state == "Z" or record.start_time != expected_start:
        return False
    if private_boundary is not None:
        if (
            record.session_id == private_boundary.session_id
            and record.process_group_id == private_boundary.process_group_id
        ):
            process_group_id = private_boundary.process_group_id
        elif private_boundary is not None and record.session_id == record.process_group_id:
            # A descendant may have called setsid() and then exited after
            # forking.  The remaining members keep the orphaned session's
            # original leader as both SID and PGID, which is still a verified
            # private group boundary when the caller is outside it.
            process_group_id = record.process_group_id
        else:
            return False
    elif record.pid == record.session_id == record.process_group_id:
        # A session leader that owns its own session and process group is a
        # private boundary established by the child-side setsid handshake.
        process_group_id = record.process_group_id
    else:
        return False
    try:
        os.killpg(process_group_id, signal_number)
    except (AttributeError, NotImplementedError, OSError, TypeError, ValueError):
        return False
    return True


def _health_signal_process_identity(
    pid: int,
    expected_start: int | None,
    signal_number: int,
    *,
    private_boundary: _HealthWorkerIdentity | None = None,
    allow_private_group_fallback: bool = True,
) -> bool:
    """Atomically bind a signal to the PID identity observed by Doctor."""

    before = _read_health_process_record(pid)
    if before is None or before.state == "Z":
        return False
    if expected_start is None or before.start_time != expected_start:
        return False
    descriptor = _health_pidfd_open(pid)
    if descriptor is None:
        if not allow_private_group_fallback:
            return False
        return _health_signal_verified_private_group(
            before,
            expected_start,
            signal_number,
            private_boundary,
        )
    try:
        after = _read_health_process_record(pid)
        if after is None or after.state == "Z" or after.start_time != expected_start:
            return False
        if _health_pidfd_send_signal(descriptor, signal_number):
            return True
    finally:
        try:
            os.close(descriptor)
        except (NotImplementedError, OSError, TypeError, ValueError):
            pass
    # A pidfd can exist while its signal operation is unavailable (for
    # example, an older libc wrapper or a transient kernel error).  Re-check
    # the same start-time identity and use only the already verified private
    # group/session boundary; never fall back to a bare PID signal.
    if not allow_private_group_fallback:
        return False
    fallback_record = _read_health_process_record(pid)
    if (
        fallback_record is None
        or fallback_record.state == "Z"
        or fallback_record.start_time != expected_start
    ):
        return False
    return _health_signal_verified_private_group(
        fallback_record,
        expected_start,
        signal_number,
        private_boundary,
    )


def _health_launch_gate_allowed(
    deadline: float,
    gate_descriptor: int,
    *,
    final_check: Callable[[], bool] | None = None,
    wait_for_permit: bool = False,
) -> bool:
    """Check a child-side launch permit immediately before exec."""

    if time.monotonic() >= deadline:
        return False
    try:
        os.set_blocking(gate_descriptor, wait_for_permit)
        permit = os.read(gate_descriptor, 1)
    except (BlockingIOError, InterruptedError):
        permit = b""
    except (NotImplementedError, OSError):
        permit = b""
    if wait_for_permit and permit != b"P":
        return False
    if not wait_for_permit and permit not in {b"", b"P"}:
        return False
    if time.monotonic() >= deadline:
        return False
    if final_check is not None and not final_check():
        return False
    return time.monotonic() < deadline


def _health_launch_gate_preexec(
    deadline: float,
    gate_descriptor: int,
    *,
    final_check: Callable[[], bool] | None = None,
) -> None:
    """Reject a forked child unless its parent grants a live launch permit."""

    try:
        allowed = _health_launch_gate_allowed(
            deadline,
            gate_descriptor,
            final_check=final_check,
            wait_for_permit=True,
        )
    finally:
        try:
            os.close(gate_descriptor)
        except (NotImplementedError, OSError, TypeError, ValueError):
            pass
    if not allowed:
        os._exit(125)


def _read_health_process_record(pid: int) -> _HealthProcessRecord | None:
    """Read the identity fields needed to distinguish a reused PID."""

    if type(pid) is not int or pid <= 0:
        return None
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except (OSError, UnicodeError):
        return None
    command_end = raw.rfind(")")
    if command_end < 0:
        return None
    fields = raw[command_end + 2 :].split()
    if len(fields) <= 19:
        return None
    try:
        return _HealthProcessRecord(
            pid,
            int(fields[1]),
            int(fields[2]),
            int(fields[3]),
            int(fields[19]),
            fields[0],
        )
    except (IndexError, TypeError, ValueError):
        return None


def _snapshot_health_processes() -> dict[int, _HealthProcessRecord] | None:
    """Take one bounded complete snapshot of the local process table."""

    # /proc is inherently racy: a process can exit between scandir() and its
    # stat read.  Retry the complete scan a small bounded number of times so a
    # transient exit does not turn an otherwise provable cleanup into a false
    # failure.  Every attempt is still all-or-nothing; a persistently partial
    # or truncated table remains an explicit failure.
    for _attempt in range(3):
        records: dict[int, _HealthProcessRecord] = {}
        numeric_entries = 0
        try:
            with os.scandir("/proc") as entries:
                for entry in entries:
                    if not entry.name.isdecimal():
                        continue
                    numeric_entries += 1
                    if numeric_entries > _HEALTH_PROC_SCAN_LIMIT:
                        break
                    record = _read_health_process_record(int(entry.name))
                    if record is None or record.pid != int(entry.name):
                        break
                    records[record.pid] = record
                else:
                    # A process table without any numeric entries is not a
                    # proof that the boundary is empty.  Treat it exactly
                    # like every other ambiguous/incomplete /proc view.
                    if numeric_entries > 0:
                        return records
        except (OSError, TypeError, ValueError):
            pass
    return None


def _health_sample_descendants(
    root_pid: int,
    tracked: dict[int, int | None],
    *,
    records: Mapping[int, _HealthProcessRecord] | None = None,
) -> tuple[dict[int, _HealthProcessRecord], bool] | None:
    """Track a root's descendants while rejecting PID reuse."""

    if records is None:
        records = _snapshot_health_processes()
    if records is None:
        return None
    children: dict[int, list[_HealthProcessRecord]] = {}
    for record in records.values():
        children.setdefault(record.parent_pid, []).append(record)

    missing = object()
    queue = [root_pid]
    queued = {root_pid}
    for pid, expected_start in tuple(tracked.items()):
        record = records.get(pid)
        if record is None:
            continue
        if expected_start is not None and expected_start != record.start_time:
            return {}, False
        if expected_start is None:
            tracked[pid] = record.start_time
        if pid not in queued:
            queue.append(pid)
            queued.add(pid)

    descendants: dict[int, _HealthProcessRecord] = {}
    visited = {root_pid}
    while queue:
        parent_pid = queue.pop()
        for record in children.get(parent_pid, ()):
            if record.pid == root_pid:
                continue
            expected_start = tracked.get(record.pid, missing)
            if expected_start is not missing:
                if expected_start is not None and expected_start != record.start_time:
                    return {}, False
            else:
                tracked[record.pid] = record.start_time
            if record.pid in visited:
                continue
            visited.add(record.pid)
            descendants[record.pid] = record
            queue.append(record.pid)

    for pid, expected_start in tuple(tracked.items()):
        record = records.get(pid)
        if record is None:
            continue
        if expected_start is not None and expected_start != record.start_time:
            return {}, False
        if expected_start is None:
            tracked[pid] = record.start_time
        descendants.setdefault(pid, record)
    return descendants, True


def _health_track_private_session_members(
    identity: _HealthWorkerIdentity,
    tracked: dict[int, int | None],
    *,
    records: dict[int, _HealthProcessRecord] | None = None,
) -> tuple[dict[int, _HealthProcessRecord], bool] | None:
    """Record every remaining member of an already-verified private session."""

    if records is None:
        records = _snapshot_health_processes()
    if records is None:
        return None
    root = records.get(identity.pid)
    if (
        not isinstance(root, _HealthProcessRecord)
        or (identity.start_time is not None and root.start_time != identity.start_time)
    ):
        return None
    members: dict[int, _HealthProcessRecord] = {}
    missing = object()
    for record in records.values():
        if record.pid == identity.pid:
            if identity.start_time is not None and record.start_time != identity.start_time:
                return {}, False
            continue
        if record.session_id != identity.session_id:
            continue
        expected_start = tracked.get(record.pid, missing)
        if expected_start is not missing and expected_start is not None and expected_start != record.start_time:
            return {}, False
        if expected_start is missing or expected_start is None:
            tracked[record.pid] = record.start_time
        members[record.pid] = record
    return members, True


def _health_sample_private_processes(
    identity: _HealthWorkerIdentity,
    tracked: dict[int, int | None],
) -> tuple[
    tuple[dict[int, _HealthProcessRecord], bool],
    tuple[dict[int, _HealthProcessRecord], bool],
] | None:
    """Take one complete process snapshot for both containment views."""

    records = _snapshot_health_processes()
    if records is None:
        return None
    sampled = _health_sample_descendants(identity.pid, tracked, records=records)
    members = _health_track_private_session_members(identity, tracked, records=records)
    if sampled is None or members is None:
        return None
    return sampled, members


def _establish_health_request_handoff() -> tuple[_HealthWorkerIdentity, dict[int, int]] | None:
    """Capture the caller identity and pre-launch descendant boundary."""

    if os.name != "posix" or _health_child_subreaper_state() is not True:
        return None
    try:
        request_pid = os.getpid()
    except (AttributeError, OSError, TypeError, ValueError):
        return None
    request_record = _read_health_process_record(request_pid)
    if request_record is None or request_record.pid != request_pid:
        return None
    records = _snapshot_health_processes()
    if not isinstance(records, Mapping) or not records:
        return None
    current_request = records.get(request_pid)
    if (
        not isinstance(current_request, _HealthProcessRecord)
        or current_request.start_time != request_record.start_time
    ):
        return None
    sampled = _health_sample_descendants(request_pid, {}, records=records)
    if sampled is None:
        return None
    descendants, valid = sampled
    if not valid:
        return None
    baseline = {pid: record.start_time for pid, record in descendants.items()}
    identity = _HealthWorkerIdentity(
        request_pid,
        current_request.session_id,
        current_request.process_group_id,
        current_request.start_time,
        child_subreaper=True,
        verified=True,
    )
    return identity, baseline


def _health_sample_request_owned_processes(
    request_identity: _HealthWorkerIdentity,
    tracked: dict[int, int | None],
    baseline: Mapping[int, int],
    *,
    excluded_identities: Mapping[int, int] | None = None,
    records: Mapping[int, _HealthProcessRecord] | None = None,
) -> tuple[dict[int, _HealthProcessRecord], bool] | None:
    """Sample only descendants created after a verified request handoff.

    The request process is intentionally not a private session leader, so its
    session membership is not an ownership boundary.  The kernel subreaper
    relationship and the complete pre-launch descendant baseline together
    define the boundary: every post-handoff descendant is owned, including a
    process adopted directly by the request after its supervisor dies.
    """

    if (
        not request_identity.verified
        or not request_identity.child_subreaper
        or type(request_identity.start_time) is not int
        or not isinstance(baseline, Mapping)
    ):
        return None
    excluded = {} if excluded_identities is None else excluded_identities
    if not isinstance(excluded, Mapping):
        return None
    if type(request_identity.pid) is not int or request_identity.pid <= 0:
        return None
    if request_identity.pid in excluded:
        return None
    for pid, start_time in baseline.items():
        if type(pid) is not int or pid <= 0 or type(start_time) is not int:
            return None
    for pid, start_time in excluded.items():
        if type(pid) is not int or pid <= 0 or type(start_time) is not int:
            return None

    if records is None:
        records = _snapshot_health_processes()
    if not isinstance(records, Mapping) or not records:
        return None
    for pid, record in records.items():
        if (
            type(pid) is not int
            or pid <= 0
            or not isinstance(record, _HealthProcessRecord)
            or record.pid != pid
        ):
            return None
    root = records.get(request_identity.pid)
    if (
        not isinstance(root, _HealthProcessRecord)
        or root.start_time != request_identity.start_time
        or root.pid != request_identity.pid
    ):
        return None

    # Validate every identity already handed to the request before accepting
    # any new process.  A reused PID is an ambiguity, not a new owned child.
    # Keep the ambiguity in the tracker as ``None``: the signal and reap
    # helpers already fail closed for that value, and retaining it prevents a
    # later sample from treating the reused PID as a fresh descendant.
    snapshot_valid = True
    for pid, expected_start in tracked.items():
        if type(pid) is not int or pid <= 0:
            return ({}, False)
        if type(expected_start) is not int:
            snapshot_valid = False
            continue
        current = records.get(pid)
        if current is not None and (
            not isinstance(current, _HealthProcessRecord)
            or current.start_time != expected_start
        ):
            tracked[pid] = None
            snapshot_valid = False

    sampled = _health_sample_descendants(
        request_identity.pid,
        {},
        records=records,
    )
    if sampled is None:
        return None
    descendants, valid = sampled
    if not valid:
        return {}, False

    for pid, expected_start in excluded.items():
        current = descendants.get(pid)
        if current is not None and current.start_time != expected_start:
            # An excluded worker identity can also be reused between the
            # supervisor report and this request-side proof.  It is never
            # owned by this boundary, but that ambiguity must not suppress
            # partitioning and cleanup of the other tracked identities.
            snapshot_valid = False

    owned: dict[int, _HealthProcessRecord] = {}
    for pid, record in descendants.items():
        if pid in tracked and tracked[pid] is None:
            # Never signal or wait on a PID after its tracked identity was
            # invalidated, even if /proc now shows a replacement process.
            snapshot_valid = False
            continue
        excluded_start = excluded.get(pid)
        if excluded_start is not None:
            if record.start_time != excluded_start:
                snapshot_valid = False
            continue
        baseline_start = baseline.get(pid)
        if baseline_start is not None and baseline_start == record.start_time:
            continue
        owned[pid] = record

    updated_tracked = dict(tracked)
    for pid, record in owned.items():
        updated_tracked[pid] = record.start_time
    tracked.clear()
    tracked.update(updated_tracked)
    return owned, snapshot_valid


def _health_signal_tracked_descendants(
    tracked: dict[int, int | None],
    signal_number: int,
    *,
    private_boundary: _HealthWorkerIdentity | None = None,
    protected_boundary: _HealthWorkerIdentity | None = None,
) -> bool:
    """Signal only descendants whose PID and /proc start time still match."""

    successful = True
    records: dict[int, _HealthProcessRecord] = {}
    escaped_groups: dict[tuple[int, int], _HealthProcessRecord] = {}
    for pid, expected_start in tuple(tracked.items()):
        record = _read_health_process_record(pid)
        if record is None:
            successful = False
            continue
        if record.state == "Z":
            continue
        if expected_start is None or expected_start != record.start_time:
            successful = False
            continue
        records[pid] = record
        if record.session_id == record.process_group_id:
            matches_private_boundary = (
                private_boundary is not None
                and record.session_id == private_boundary.session_id
                and record.process_group_id == private_boundary.process_group_id
            )
            matches_protected_boundary = (
                protected_boundary is not None
                and record.session_id == protected_boundary.session_id
                and record.process_group_id == protected_boundary.process_group_id
            )
            if not matches_private_boundary and not matches_protected_boundary:
                escaped_groups[(record.session_id, record.process_group_id)] = record

    signalled_groups: set[tuple[int, int]] = set()
    for pid, record in records.items():
        if private_boundary is not None and (
            record.session_id == private_boundary.session_id
            and record.process_group_id == private_boundary.process_group_id
        ):
            if not _health_signal_process_identity(
                pid,
                record.start_time,
                signal_number,
                private_boundary=private_boundary,
            ):
                successful = False
            continue
        if protected_boundary is not None and (
            record.session_id == protected_boundary.session_id
            and record.process_group_id == protected_boundary.process_group_id
        ):
            if not _health_signal_process_identity(
                pid,
                record.start_time,
                signal_number,
            ):
                successful = False
            continue
        group_key = (record.session_id, record.process_group_id)
        leader = escaped_groups.get(group_key)
        if leader is not None:
            if group_key in signalled_groups:
                continue
            signalled_groups.add(group_key)
            if not _health_signal_verified_private_group(
                leader,
                leader.start_time,
                signal_number,
                private_boundary=private_boundary,
            ):
                successful = False
            continue
        if not _health_signal_process_identity(
            pid,
            record.start_time,
            signal_number,
            private_boundary=private_boundary,
        ):
            successful = False
    return successful


def _health_tracked_processes_absent(tracked: dict[int, int | None]) -> bool:
    """Confirm that every tracked identity is gone, not merely unreadable."""

    for pid, expected_start in tuple(tracked.items()):
        record = _read_health_process_record(pid)
        if record is not None:
            if expected_start is None or expected_start != record.start_time:
                return False
            return False
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            continue
        except OSError as exc:
            if exc.errno == errno.ESRCH:
                continue
            return False
        except (AttributeError, TypeError, ValueError):
            return False
        return False
    return True


def _health_reap_tracked_children(
    tracked: dict[int, int | None],
    *,
    owner_pid: int | None = None,
    reaped: set[tuple[int, int]] | None = None,
    pending: set[int] | None = None,
    failed: set[int] | None = None,
    not_owned: set[int] | None = None,
    allow_absent: bool = False,
) -> bool:
    """Reap adopted descendants without ever waiting indefinitely.

    ``allow_absent`` is reserved for a complete request-side snapshot: when
    the identity is no longer readable and ``kill(pid, 0)`` proves ESRCH, the
    tracker can discard it without attempting a PID-based wait.
    """

    successful = True
    for pid in tuple(tracked):
        expected_start = tracked.get(pid)
        if type(expected_start) is not int:
            successful = False
            if failed is not None:
                failed.add(pid)
            continue
        record = _read_health_process_record(pid)
        if record is None:
            if allow_absent:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    tracked.pop(pid, None)
                    continue
                except OSError as exc:
                    if exc.errno == errno.ESRCH:
                        tracked.pop(pid, None)
                        continue
                except (AttributeError, TypeError, ValueError):
                    pass
            if owner_pid is not None and not_owned is not None:
                not_owned.add(pid)
            successful = False
            if failed is not None:
                failed.add(pid)
            continue
        # waitpid() is PID-based.  Re-read the kernel identity immediately
        # before it so a stale tracked PID can never reap a replacement child.
        if record.start_time != expected_start:
            successful = False
            if failed is not None:
                failed.add(pid)
            continue
        if owner_pid is not None:
            if record.parent_pid != owner_pid:
                if not_owned is not None:
                    not_owned.add(pid)
                continue
            elif not_owned is not None:
                not_owned.discard(pid)
        try:
            waited_pid, _status = os.waitpid(pid, os.WNOHANG)
            if waited_pid != pid:
                successful = False
                if pending is not None and waited_pid == 0:
                    pending.add(pid)
                elif failed is not None:
                    failed.add(pid)
            else:
                if reaped is not None:
                    reaped.add((pid, expected_start))
                _HEALTH_WORKER_REAPED_PROBES.add((pid, expected_start))
                tracked.pop(pid, None)
        except ChildProcessError:
            successful = False
            if failed is not None:
                failed.add(pid)
        except OSError as exc:
            successful = False
            if failed is not None:
                failed.add(pid)
        except Exception:
            successful = False
            if failed is not None:
                failed.add(pid)
    return successful


def _health_apply_reap_notifications(
    tracked: dict[int, int | None],
    reaped: set[tuple[int, int]],
    raw: object,
) -> bool:
    """Apply only wait-proven reap notifications bound to the tracked identity."""

    if not isinstance(raw, Mapping):
        return False
    notifications = raw.get("reaped")
    if isinstance(notifications, (str, bytes)) or not isinstance(notifications, Sequence):
        return False
    valid = True
    for item in notifications:
        waited = True
        if isinstance(item, Mapping):
            pid = item.get("pid")
            start_time = item.get("start_time")
            waited = item.get("waited")
        elif isinstance(item, Sequence) and not isinstance(item, (str, bytes)) and len(item) == 2:
            pid, start_time = item
        else:
            valid = False
            continue
        if (
            type(pid) is not int
            or pid <= 0
            or type(start_time) is not int
            or waited is not True
        ):
            valid = False
            continue
        if tracked.get(pid) != start_time:
            # A reused PID must remain tracked under its newer identity.
            continue
        identity = (pid, start_time)
        reaped.add(identity)
        tracked.pop(pid, None)
    return valid


def _health_cleanup_boundary(
    identity: _HealthWorkerIdentity,
    tracked: dict[int, int | None],
    timeout: float,
    *,
    owner_pid: int | None = None,
) -> bool:
    """Boundedly kill and verify all descendants of a private worker session."""

    if not identity.verified or not _HEALTH_WORKER_CONTAINMENT_VALID:
        return False
    try:
        cleanup_timeout = float(timeout)
    except (TypeError, ValueError):
        return False
    if not math.isfinite(cleanup_timeout) or cleanup_timeout < 0:
        return False
    deadline = time.monotonic() + min(cleanup_timeout, MAX_WORKER_CLEANUP_SECONDS)
    reap_owner_pid = identity.pid if owner_pid is None else owner_pid
    successful = True
    reaped: set[tuple[int, int]] = set()
    reap_failures: set[int] = set()
    reap_pending: set[int] = set()
    not_owned: set[int] = set()

    while True:
        snapshot = _health_sample_private_processes(identity, tracked)
        if snapshot is None:
            # An incomplete process view cannot prove that the boundary is
            # empty.  Still use the identities already established by an
            # earlier complete view: signal them and make any bounded reap
            # attempt, then keep the result explicitly unconfirmed.
            successful = False
            if not _health_signal_tracked_descendants(
                tracked,
                signal.SIGKILL,
                private_boundary=identity if owner_pid is not None else None,
                protected_boundary=identity if owner_pid is None else None,
            ):
                successful = False
            _health_reap_tracked_children(
                tracked,
                owner_pid=reap_owner_pid,
                reaped=reaped,
                pending=reap_pending,
                failed=reap_failures,
                not_owned=not_owned,
            )
            if reap_failures:
                successful = False
            if time.monotonic() >= deadline:
                break
            time.sleep(min(0.005, max(0.0, deadline - time.monotonic())))
            continue
        sampled, members = snapshot
        descendants, sampled_ok = sampled
        session_members, members_ok = members
        if not sampled_ok or not members_ok:
            successful = False
        all_members = {**descendants, **session_members}
        for pid, record in all_members.items():
            if record.parent_pid == reap_owner_pid:
                not_owned.discard(pid)
            else:
                not_owned.add(pid)
        live = {pid: record for pid, record in all_members.items() if record.state != "Z"}
        if not live:
            _health_reap_tracked_children(
                tracked,
                owner_pid=reap_owner_pid,
                reaped=reaped,
                pending=reap_pending,
                failed=reap_failures,
                not_owned=not_owned,
            )
            if reap_failures:
                successful = False
            snapshot = _health_sample_private_processes(identity, tracked)
            if snapshot is None:
                return False
            sampled, members = snapshot
            descendants, sampled_ok = sampled
            session_members, members_ok = members
            if not sampled_ok or not members_ok:
                successful = False
            live = {
                pid: record
                for pid, record in {**descendants, **session_members}.items()
                if record.state != "Z"
            }
            if not live and _health_tracked_processes_absent(tracked):
                return successful
        if not _health_signal_tracked_descendants(
            tracked,
            signal.SIGKILL,
            private_boundary=identity if owner_pid is not None else None,
            protected_boundary=identity if owner_pid is None else None,
        ):
            successful = False
        _health_reap_tracked_children(
            tracked,
            owner_pid=reap_owner_pid,
            reaped=reaped,
            pending=reap_pending,
            failed=reap_failures,
            not_owned=not_owned,
        )
        if reap_failures:
            successful = False
        if time.monotonic() >= deadline:
            break
        time.sleep(min(0.005, max(0.0, deadline - time.monotonic())))

    snapshot = _health_sample_private_processes(identity, tracked)
    if snapshot is None:
        return False
    sampled, members = snapshot
    descendants, sampled_ok = sampled
    session_members, members_ok = members
    live = {
        pid: record
        for pid, record in {**descendants, **session_members}.items()
        if record.state != "Z"
    }
    return successful and sampled_ok and members_ok and not live and _health_tracked_processes_absent(tracked)


def _health_cleanup_request_boundary(
    request_identity: _HealthWorkerIdentity,
    baseline: Mapping[int, int],
    tracked: dict[int, int | None],
    timeout: float,
    *,
    excluded_identities: Mapping[int, int] | None = None,
    private_boundary: _HealthWorkerIdentity | None = None,
    require_empty: bool = False,
) -> bool:
    """Contain descendants adopted by the request after supervisor loss.

    The request identity and baseline are captured before the supervisor is
    launched.  A complete snapshot can therefore distinguish a new adopted
    descendant from an existing request child, even after a probe has called
    ``setsid()`` and escaped the supervisor's session.  Every incomplete view
    keeps cleanup unconfirmed, while already tracked identities still receive
    the best identity-bound signal available.
    """

    if (
        not request_identity.verified
        or not request_identity.child_subreaper
        or type(request_identity.pid) is not int
        or request_identity.pid <= 0
        or type(request_identity.session_id) is not int
        or request_identity.session_id <= 0
        or type(request_identity.process_group_id) is not int
        or request_identity.process_group_id <= 0
        or type(request_identity.start_time) is not int
        or request_identity.start_time <= 0
        or not isinstance(baseline, Mapping)
    ):
        return False
    if private_boundary is not None and (
        not private_boundary.verified
        or type(private_boundary.pid) is not int
        or private_boundary.pid <= 0
        or type(private_boundary.session_id) is not int
        or private_boundary.session_id <= 0
        or type(private_boundary.process_group_id) is not int
        or private_boundary.process_group_id <= 0
        or type(private_boundary.start_time) is not int
        or private_boundary.start_time <= 0
    ):
        return False
    if type(require_empty) is not bool:
        return False
    try:
        cleanup_timeout = float(timeout)
    except (TypeError, ValueError):
        return False
    if not math.isfinite(cleanup_timeout) or cleanup_timeout < 0:
        return False
    excluded = {} if excluded_identities is None else excluded_identities
    if not isinstance(excluded, Mapping):
        return False
    deadline = time.monotonic() + min(cleanup_timeout, MAX_WORKER_CLEANUP_SECONDS)
    reaped: set[tuple[int, int]] = set()
    reap_pending: set[int] = set()
    reap_failures: set[int] = set()
    not_owned: set[int] = set()
    signal_failed = False
    signal_attempted = False
    signal_settled = False
    final_proof_violation = False
    stable_empty = False

    while True:
        sampled = _health_sample_request_owned_processes(
            request_identity,
            tracked,
            baseline,
            excluded_identities=excluded,
        )
        if sampled is None:
            stable_empty = False
            if tracked:
                signal_attempted = True
                if not _health_signal_tracked_descendants(
                    tracked,
                    signal.SIGKILL,
                    private_boundary=private_boundary,
                    protected_boundary=request_identity,
                ):
                    signal_failed = True
                if signal_attempted and not signal_settled and cleanup_timeout > 0:
                    # SIGKILL is delivered synchronously to the target group,
                    # but a target may not become waitable until it next
                    # reaches the kernel.  Reserve one small, bounded settle
                    # window before the identity-bound reap pass so a tight
                    # cleanup budget cannot strand a zombie.
                    time.sleep(min(0.005, cleanup_timeout))
                    signal_settled = True
                _health_reap_tracked_children(
                    tracked,
                    owner_pid=request_identity.pid,
                    reaped=reaped,
                    pending=reap_pending,
                    failed=reap_failures,
                    not_owned=not_owned,
                )
        else:
            owned, snapshot_valid = sampled
            live = {pid: record for pid, record in owned.items() if record.state != "Z"}
            if require_empty and owned:
                # A confirmed supervisor report is only reusable as a PASS
                # when this final request-side proof starts empty.  We still
                # contain and reap anything discovered here, but the late
                # descendant makes the observation explicitly UNKNOWN.
                final_proof_violation = True
            if any(type(start_time) is not int for start_time in tracked.values()):
                snapshot_valid = False
            if not snapshot_valid:
                stable_empty = False
            signalable_tracked = any(type(start_time) is int for start_time in tracked.values())
            if live or (not snapshot_valid and signalable_tracked):
                stable_empty = False
                signal_attempted = True
                if not _health_signal_tracked_descendants(
                    tracked,
                    signal.SIGKILL,
                    private_boundary=private_boundary,
                    protected_boundary=request_identity,
                ):
                    signal_failed = True
            if signal_attempted and not signal_settled and cleanup_timeout > 0:
                # SIGKILL is delivered synchronously to the target group, but
                # a target may not become waitable until it next reaches the
                # kernel.  Reserve one small, bounded settle window before
                # the identity-bound reap pass so a tight cleanup budget
                # cannot strand a zombie.
                time.sleep(min(0.005, cleanup_timeout))
                signal_settled = True
            reap_ok = _health_reap_tracked_children(
                tracked,
                owner_pid=request_identity.pid,
                reaped=reaped,
                pending=reap_pending,
                failed=reap_failures,
                not_owned=not_owned,
                allow_absent=snapshot_valid,
            )
            if (
                snapshot_valid
                and not live
                and reap_ok
                and not reap_failures
                and not signal_failed
                and _health_tracked_processes_absent(tracked)
            ):
                if stable_empty:
                    return not final_proof_violation
                stable_empty = True
            else:
                stable_empty = False

        if time.monotonic() >= deadline:
            break
        time.sleep(min(0.005, max(0.0, deadline - time.monotonic())))
    return False


def _health_register_probe(pid: int) -> bool:
    """Register a newly launched probe before it can fork away."""

    global _HEALTH_WORKER_CONTAINMENT_VALID
    if type(pid) is not int or pid <= 0:
        _HEALTH_WORKER_CONTAINMENT_VALID = False
        return False
    missing = object()
    existing_start = _HEALTH_WORKER_TRACKED_DESCENDANTS.get(pid, missing)
    if existing_start is not missing:
        if existing_start is None:
            _HEALTH_WORKER_CONTAINMENT_VALID = False
            return False
        record = _read_health_process_record(pid)
        if record is None:
            # The probe was observed and identity-bound before it exited.
            # A post-Popen registration must not turn a normal fast exit into
            # a containment failure.
            return True
        if record.start_time != existing_start:
            _HEALTH_WORKER_CONTAINMENT_VALID = False
            return False
        return True
    record = _read_health_process_record(pid)
    if record is not None:
        _HEALTH_WORKER_TRACKED_DESCENDANTS[pid] = record.start_time
        return True
    _HEALTH_WORKER_CONTAINMENT_VALID = False
    _HEALTH_WORKER_TRACKED_DESCENDANTS[pid] = None
    return False


def _health_observe_worker_identity(process: Any) -> _HealthWorkerIdentity | None:
    """Observe a worker only when it is its own session and group leader."""

    pid = getattr(process, "pid", None)
    if type(pid) is not int or pid <= 0:
        return None
    try:
        session_id = os.getsid(pid)
        process_group_id = os.getpgid(pid)
    except (AttributeError, OSError, ProcessLookupError, TypeError, ValueError):
        return None
    if session_id != pid or process_group_id != pid:
        return None
    record = _read_health_process_record(pid)
    return _HealthWorkerIdentity(
        pid,
        session_id,
        process_group_id,
        record.start_time if record is not None else None,
        verified=record is not None,
    )


def _health_identity_matches(
    observed: _HealthWorkerIdentity,
    advertised: _HealthWorkerIdentity | None,
) -> bool:
    if not observed.verified:
        return False
    if advertised is None:
        return True
    if (
        observed.pid != advertised.pid
        or observed.session_id != advertised.session_id
        or observed.process_group_id != advertised.process_group_id
    ):
        return False
    return advertised.start_time is None or advertised.start_time == observed.start_time


def _health_group_is_absent(identity: _HealthWorkerIdentity) -> bool:
    if not identity.verified or not _HEALTH_WORKER_CONTAINMENT_VALID:
        return False
    members = _health_track_private_session_members(identity, {})
    if members is None:
        return False
    session_members, valid = members
    return valid and not any(record.state != "Z" for record in session_members.values())


def _terminate_process(process: subprocess.Popen[bytes]) -> None:
    """Kill a bounded process and descendants without killing its worker."""

    # The default direct-operation path owns a fresh session.  A forked
    # Doctor worker instead owns the process group inherited by its probe; the
    # request-side worker reaper kills that group because killing it here would
    # also kill this function's worker.
    if not _HEALTH_WORKER_GROUP_OWNED:
        try:
            process_group = os.getpgid(process.pid)
            process_session = os.getsid(process.pid)
            if process_group == process.pid and process_session == process.pid:
                os.killpg(process_group, signal.SIGKILL)
                return
        except (AttributeError, OSError, ProcessLookupError, TypeError, ValueError):
            pass
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
    gate_read: int | None = None
    gate_write: int | None = None
    pid_read: int | None = None
    pid_write: int | None = None
    launch_pid: int | None = None
    launch_start: int | None = None
    launch_gate_failed = [False]
    launch_signal_failed = [False]
    gate_stop: threading.Event | None = None
    gate_thread: threading.Thread | None = None

    def stop_launch_gate() -> None:
        nonlocal gate_read, gate_write, pid_read, pid_write, gate_thread
        if gate_stop is not None:
            gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=0.1)
            gate_thread = None
        for descriptor in (gate_read, gate_write, pid_read, pid_write):
            if isinstance(descriptor, int):
                try:
                    os.close(descriptor)
                except OSError:
                    pass
        gate_read = gate_write = pid_read = pid_write = None

    if not _health_process_boundary_owned():
        return CommandObservation(
            argv,
            error_code="worker_boundary_required",
            error_detail="default process operations require an owned Doctor supervisor boundary",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )

    if _operation_cancelled(operation_deadline, cancellation):
        return CommandObservation(
            argv,
            timed_out=True,
            error_code="timeout",
            error_detail="process launch was cancelled before its deadline",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )

    try:
        # The child reports its PID before any potentially blocked pre-exec
        # hook.  The watcher can then use an identity-bound pidfd to stop it
        # even while Popen is still waiting for exec-error synchronization.
        if _operation_cancelled(operation_deadline, cancellation):
            return CommandObservation(
                argv,
                timed_out=True,
                error_code="timeout",
                error_detail="process launch was cancelled before its deadline",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        try:
            gate_read, gate_write = os.pipe()
            pid_read, pid_write = os.pipe()
            os.set_blocking(gate_read, False)
            os.set_blocking(pid_read, False)
        except (AttributeError, NotImplementedError, OSError, TypeError, ValueError) as exc:
            for descriptor in (gate_read, gate_write, pid_read, pid_write):
                if isinstance(descriptor, int):
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
            gate_read = gate_write = pid_read = pid_write = None
            return CommandObservation(
                argv,
                error_code="launch_gate_unavailable",
                error_detail=type(exc).__name__,
                duration_ms=(time.monotonic() - started) * 1000.0,
            )

        gate_stop = threading.Event()
        assert gate_read is not None
        assert gate_write is not None
        assert pid_read is not None
        assert pid_write is not None
        write_gate = gate_write
        read_pid = pid_read
        write_pid = pid_write

        def write_gate_token(token: bytes) -> None:
            try:
                os.write(write_gate, token)
            except (BrokenPipeError, OSError):
                pass

        def watch_launch_gate() -> None:
            nonlocal launch_pid, launch_start
            permit_sent = False
            while not gate_stop.is_set():
                if launch_pid is None:
                    try:
                        raw_pid = os.read(read_pid, 32)
                    except (BlockingIOError, InterruptedError):
                        raw_pid = b""
                    except (NotImplementedError, OSError):
                        launch_gate_failed[0] = True
                        raw_pid = b""
                    if raw_pid:
                        try:
                            candidate_pid = int(raw_pid.decode("ascii"))
                        except (UnicodeError, ValueError):
                            launch_gate_failed[0] = True
                        else:
                            record = _read_health_process_record(candidate_pid)
                            if record is None:
                                launch_gate_failed[0] = True
                            else:
                                launch_pid = candidate_pid
                                launch_start = record.start_time
                                # Establish the identity before releasing the
                                # child-side gate.  Registration itself is
                                # repeated after Popen returns so worker-loss
                                # hooks cannot strand a child between launch
                                # and tracker ownership, but the gate must not
                                # depend on a callback that can terminate the
                                # current worker before Popen completes.
                                _HEALTH_WORKER_TRACKED_DESCENDANTS[candidate_pid] = record.start_time
                cancelled = (
                    cancellation is not None and cancellation.is_cancelled()
                ) or time.monotonic() >= operation_deadline
                if launch_gate_failed[0]:
                    write_gate_token(b"C")
                    return
                if cancelled:
                    write_gate_token(b"C")
                    if launch_pid is not None and launch_start is not None:
                        if not _health_signal_process_identity(
                            launch_pid,
                            launch_start,
                            signal.SIGKILL,
                            allow_private_group_fallback=False,
                        ):
                            launch_signal_failed[0] = True
                    return
                if launch_pid is not None and not permit_sent:
                    write_gate_token(b"P")
                    permit_sent = True
                gate_stop.wait(min(0.005, max(0.0, operation_deadline - time.monotonic())))

        gate_thread = threading.Thread(
            target=watch_launch_gate,
            name="realmheart-doctor-launch-gate",
            daemon=True,
        )
        gate_thread.start()

        def stop_launch_gate() -> None:
            nonlocal gate_read, gate_write, pid_read, pid_write, gate_thread
            if gate_stop is not None:
                gate_stop.set()
            if gate_thread is not None:
                gate_thread.join(timeout=0.1)
                gate_thread = None
            for descriptor in (gate_read, gate_write, pid_read, pid_write):
                if isinstance(descriptor, int):
                    try:
                        os.close(descriptor)
                    except OSError:
                        pass
            gate_read = gate_write = pid_read = pid_write = None

        def launch_gate() -> None:
            assert gate_read is not None
            assert pid_write is not None
            try:
                os.write(pid_write, str(os.getpid()).encode("ascii"))
            except (BrokenPipeError, OSError):
                os._exit(125)
            finally:
                try:
                    os.close(pid_write)
                except OSError:
                    pass
            if cancellation is None:
                final_check: Callable[[], bool] | None = None
            else:
                launch_cancellation = cancellation
                final_check = lambda: not launch_cancellation.is_cancelled()
            _health_launch_gate_preexec(
                operation_deadline,
                gate_read,
                final_check=final_check,
            )
            if _HEALTH_WORKER_GROUP_OWNED:
                _health_probe_preexec()
            if cancellation is not None and cancellation.is_cancelled():
                os._exit(125)
            if time.monotonic() >= operation_deadline:
                os._exit(125)

        popen_options: dict[str, Any] = {
            "stdin": subprocess.DEVNULL,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            "shell": False,
            "close_fds": True,
            "start_new_session": not _HEALTH_WORKER_GROUP_OWNED,
            "env": dict(_SANITIZED_ENVIRONMENT),
            "preexec_fn": launch_gate,
        }
        if executable_fd is None:
            assert gate_read is not None
            assert pid_write is not None
            popen_options["pass_fds"] = (gate_read, pid_write)
            process = cast(subprocess.Popen[bytes], subprocess.Popen(argv, **popen_options))
        else:
            # Python exposes fd-backed exec through ``os.execve`` on some
            # POSIX builds but does not expose it as a Popen argument.  The
            # proc-fd executable path is safe here because the descriptor is
            # opened with O_NOFOLLOW, kept alive with pass_fds, and never
            # resolved through PATH.
            assert gate_read is not None
            assert pid_write is not None
            popen_options["pass_fds"] = (executable_fd, gate_read, pid_write)
            process = cast(
                subprocess.Popen[bytes],
                subprocess.Popen(
                    argv,
                    executable=f"/proc/self/fd/{executable_fd}",
                    **popen_options,
                ),
            )
        assert process is not None
        stop_launch_gate()
        if launch_gate_failed[0]:
            _terminate_process(process)
            _wait_process(process, 0.1)
            return CommandObservation(
                argv,
                timed_out=_operation_cancelled(operation_deadline, cancellation),
                error_code="launch_gate_denied",
                error_detail="worker identity could not be established before launch",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        if launch_signal_failed[0]:
            _terminate_process(process)
            _wait_process(process, 0.1)
            return CommandObservation(
                argv,
                timed_out=True,
                error_code="launch_cleanup_failed",
                error_detail="worker identity-bound termination failed",
                duration_ms=(time.monotonic() - started) * 1000.0,
            )
        if _HEALTH_WORKER_GROUP_OWNED:
            _health_register_probe(process.pid)
    except subprocess.SubprocessError as exc:
        stop_launch_gate()
        cancelled = _operation_cancelled(operation_deadline, cancellation)
        return CommandObservation(
            argv,
            timed_out=cancelled,
            error_code=("timeout" if cancelled else "launch_gate_denied"),
            error_detail=type(exc).__name__,
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except FileNotFoundError:
        stop_launch_gate()
        return CommandObservation(
            argv,
            error_code=("descriptor_execution_unavailable" if executable_fd is not None else "executable_missing"),
            error_detail="FileNotFoundError",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except PermissionError:
        stop_launch_gate()
        return CommandObservation(
            argv,
            error_code="permission_denied",
            error_detail="PermissionError",
            duration_ms=(time.monotonic() - started) * 1000.0,
        )
    except OSError as exc:
        stop_launch_gate()
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
        if gate_stop is not None:
            gate_stop.set()
        if gate_thread is not None:
            gate_thread.join(timeout=0.01)
        for descriptor_name in ("gate_read", "gate_write"):
            descriptor = locals().get(descriptor_name)
            if isinstance(descriptor, int):
                try:
                    os.close(descriptor)
                except OSError:
                    pass
        if process.poll() is None:
            _terminate_process(process)
            _wait_process(process, MAX_WORKER_CLEANUP_SECONDS)
        if process.returncode is not None and _HEALTH_WORKER_GROUP_OWNED:
            expected_start = _HEALTH_WORKER_TRACKED_DESCENDANTS.pop(process.pid, None)
            if type(expected_start) is int:
                _HEALTH_WORKER_REAPED_PROBES.add((process.pid, expected_start))
        if _HEALTH_WORKER_GROUP_OWNED and _HEALTH_WORKER_IDENTITY is not None:
            if not _health_cleanup_boundary(
                _HEALTH_WORKER_IDENTITY,
                _HEALTH_WORKER_TRACKED_DESCENDANTS,
                MAX_WORKER_CLEANUP_SECONDS,
            ):
                error_code = "worker_cleanup_incomplete"
                error_detail = "probe descendant containment could not be confirmed"
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
        if not _health_process_boundary_owned():
            return CommandObservation(
                command,
                error_code="worker_boundary_required",
                error_detail="default process operations require an owned Doctor supervisor boundary",
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


def _establish_health_worker_group() -> bool:
    """Give one forked worker an exclusive, descendant-trackable session."""

    global _HEALTH_WORKER_CONTAINMENT_VALID
    global _HEALTH_WORKER_GROUP_OWNED, _HEALTH_WORKER_IDENTITY
    _HEALTH_WORKER_GROUP_OWNED = False
    _HEALTH_WORKER_IDENTITY = None
    _HEALTH_WORKER_TRACKED_DESCENDANTS.clear()
    _HEALTH_WORKER_REAPED_PROBES.clear()
    _HEALTH_WORKER_CONTAINMENT_VALID = True
    try:
        # The parent must never call setpgid here: making the worker a group
        # leader first would make this setsid handshake fail with EPERM.
        os.setsid()
    except (AttributeError, OSError, ProcessLookupError):
        return False
    try:
        pid = os.getpid()
        session_id = os.getsid(pid)
        process_group_id = os.getpgid(pid)
    except (AttributeError, OSError, ProcessLookupError, TypeError, ValueError):
        return False
    if session_id != pid or process_group_id != pid:
        return False
    if not _enable_health_child_subreaper():
        return False
    if not _health_prctl(_HEALTH_PR_SET_PDEATHSIG, signal.SIGKILL):
        return False
    child_subreaper = _health_child_subreaper_state()
    if child_subreaper is not True:
        return False
    record = _read_health_process_record(pid)
    _HEALTH_WORKER_IDENTITY = _HealthWorkerIdentity(
        pid,
        session_id,
        process_group_id,
        record.start_time if record is not None else None,
        child_subreaper=child_subreaper,
        verified=record is not None,
    )
    _HEALTH_WORKER_GROUP_OWNED = True
    return True


def _health_worker_ready_message() -> dict[str, object] | bool:
    identity = _HEALTH_WORKER_IDENTITY
    if not _HEALTH_WORKER_GROUP_OWNED or identity is None:
        return False
    return {
        "pid": identity.pid,
        "session_id": identity.session_id,
        "process_group_id": identity.process_group_id,
        "start_time": identity.start_time,
        "child_subreaper": identity.child_subreaper,
    }


def _health_ready_identity(raw: object) -> tuple[_HealthWorkerIdentity | None, bool]:
    """Decode the readiness proof without treating a bare group bool as proof."""

    if raw is None or raw is True:
        return None, False
    if raw is False or not isinstance(raw, Mapping):
        return None, False
    pid = raw.get("pid")
    session_id = raw.get("session_id")
    process_group_id = raw.get("process_group_id")
    start_time = raw.get("start_time")
    child_subreaper = raw.get("child_subreaper")
    if (
        type(pid) is not int
        or pid <= 0
        or type(session_id) is not int
        or type(process_group_id) is not int
        or (start_time is not None and type(start_time) is not int)
        or child_subreaper is not True
    ):
        return None, False
    return (
        _HealthWorkerIdentity(
            pid,
            session_id,
            process_group_id,
            start_time,
            child_subreaper=True,
        ),
        True,
    )


def _claim_health_worker_group(
    process: Any,
    advertised: _HealthWorkerIdentity | None = None,
) -> bool:
    """Verify the worker's private session without changing its process group."""

    observed = _health_observe_worker_identity(process)
    return observed is not None and _health_identity_matches(observed, advertised)


def _signal_health_worker_direct(process: Any, signal_number: int) -> bool:
    pid = getattr(process, "pid", None)
    if type(pid) is int and pid > 0:
        record = _read_health_process_record(pid)
        if record is None:
            return False
        return _health_signal_process_identity(pid, record.start_time, signal_number)
    try:
        method = getattr(process, "kill" if signal_number == signal.SIGKILL else "terminate", None)
    except Exception:
        return False
    if not callable(method):
        return False
    try:
        method()
    except Exception:
        return False
    return True


def _signal_health_worker(
    process: Any,
    signal_number: int,
    *,
    process_group_owned: bool,
    worker_identity: _HealthWorkerIdentity | None = None,
) -> bool:
    """Signal a verified group and report whether the requested signal landed."""

    if not process_group_owned:
        return _signal_health_worker_direct(process, signal_number)
    if worker_identity is None or not worker_identity.verified:
        return False
    try:
        observed = _health_observe_worker_identity(process)
    except Exception:
        observed = None
    if observed is None or not _health_identity_matches(observed, worker_identity):
        return False
    return _health_signal_process_identity(
        worker_identity.pid,
        worker_identity.start_time,
        signal_number,
        private_boundary=worker_identity,
    )


def _reap_health_worker(
    process: Any,
    cleanup_seconds: float,
    *,
    process_group_owned: bool = False,
    worker_identity: _HealthWorkerIdentity | None = None,
    tracked_descendants: dict[int, int | None] | None = None,
    shutdown_event: Any = None,
    cleanup_connection: Any = None,
    fallback_owner_pid: int | None = None,
    request_identity: _HealthWorkerIdentity | None = None,
    request_baseline: Mapping[int, int] | None = None,
    require_final_request_proof: bool = True,
) -> bool:
    """Stop and boundedly reap a worker, its private session, and escaped children.

    A confirmed supervisor report can be reused for a PASS only when
    ``require_final_request_proof`` also passes the request-side strict-empty
    boundary proof.
    """

    try:
        cleanup_timeout = min(max(0.0, float(cleanup_seconds)), MAX_WORKER_CLEANUP_SECONDS)
    except (TypeError, ValueError):
        cleanup_timeout = 0.0
        cleanup_valid = False
    else:
        cleanup_valid = math.isfinite(cleanup_timeout)
    if not cleanup_valid:
        cleanup_timeout = 0.0
    if type(require_final_request_proof) is not bool:
        return False
    tracked = tracked_descendants if tracked_descendants is not None else {}
    successful = cleanup_valid
    worker_stop_confirmed = cleanup_valid
    if process_group_owned and (worker_identity is None or not worker_identity.verified):
        successful = False
        worker_stop_confirmed = False
    if process_group_owned and shutdown_event is not None:
        try:
            shutdown_event.set()
        except (OSError, RuntimeError, ValueError):
            successful = False
            worker_stop_confirmed = False

    try:
        alive = bool(process.is_alive())
    except Exception:
        alive = True
        successful = False
        worker_stop_confirmed = False

    def request_side_cleanup(
        *,
        require_request_boundary: bool = False,
        require_empty: bool = False,
    ) -> bool:
        if worker_identity is None:
            return False
        try:
            if request_identity is not None or request_baseline is not None:
                if request_identity is None or request_baseline is None:
                    return False
                if type(worker_identity.start_time) is not int:
                    return False
                excluded = {worker_identity.pid: worker_identity.start_time}
                return _health_cleanup_request_boundary(
                    request_identity,
                    request_baseline,
                    tracked,
                    cleanup_timeout,
                    excluded_identities=excluded,
                    private_boundary=worker_identity,
                    require_empty=require_empty,
                )
            if require_request_boundary:
                return False
            return _health_cleanup_boundary(
                worker_identity,
                tracked,
                cleanup_timeout,
                owner_pid=fallback_owner_pid if fallback_owner_pid is not None else os.getpid(),
            )
        except BaseException:
            return False

    if process_group_owned and shutdown_event is not None:
        # The supervisor owns the descendant tree.  Give it a bounded window
        # to perform the identity-bound signal/reap pass and publish proof.
        try:
            process.join(timeout=min(MAX_WORKER_CLEANUP_SECONDS * 2, 0.5))
        except Exception:
            successful = False
            worker_stop_confirmed = False
        try:
            alive = bool(process.is_alive())
        except Exception:
            alive = True
            successful = False
            worker_stop_confirmed = False
        report: object | None = None
        if cleanup_connection is not None:
            try:
                if cleanup_connection.poll(0):
                    report = cleanup_connection.recv()
            except (EOFError, OSError, ValueError):
                report = None
        report_received = isinstance(report, Mapping)
        if report_received:
            confirmed = report.get("confirmed")
            raw_tracked = report.get("tracked")
            if type(confirmed) is not bool:
                successful = False
            else:
                successful = successful and confirmed
            if isinstance(raw_tracked, Mapping):
                missing = object()
                for pid, start_time in raw_tracked.items():
                    if type(pid) is not int or pid <= 0 or (start_time is not None and type(start_time) is not int):
                        continue
                    existing_start = tracked.get(pid, missing)
                    if existing_start is None:
                        # A request-side identity mismatch is sticky.  A
                        # later supervisor report must not re-authorize the
                        # reused PID for signaling or waiting.
                        continue
                    if type(existing_start) is int and type(start_time) is int and existing_start != start_time:
                        tracked[pid] = None
                    else:
                        tracked[pid] = start_time
            else:
                successful = False
        else:
            # A dead supervisor cannot lend us its in-memory tracker.  The
            # request process owns a temporary subreaper boundary and keeps a
            # separately sampled identity map, so perform a bounded fallback
            # instead of treating the missing report as proof of cleanup.
            successful = False
            if not alive:
                return (
                    cleanup_valid
                    and worker_stop_confirmed
                    and request_side_cleanup(require_empty=require_final_request_proof)
                )
        if alive:
            if not _signal_health_worker(
                process,
                signal.SIGKILL,
                process_group_owned=True,
                worker_identity=worker_identity,
            ):
                successful = False
                worker_stop_confirmed = False
            try:
                process.join(timeout=cleanup_timeout)
            except Exception:
                successful = False
                worker_stop_confirmed = False
            try:
                alive = bool(process.is_alive())
            except Exception:
                alive = True
                successful = False
                worker_stop_confirmed = False
        if not alive and not report_received:
            return (
                cleanup_valid
                and worker_stop_confirmed
                and request_side_cleanup(require_empty=require_final_request_proof)
            )
        if not alive and report_received:
            # A supervisor's confirmed report covers only the boundary it
            # could observe.  The request-side subreaper must perform its own
            # bounded, strict-empty proof before a PASS payload is reusable;
            # this catches descendants created or reparented in the report
            # handoff window and fails closed on an incomplete /proc view.
            final_request_proof = request_side_cleanup(
                require_request_boundary=True,
                require_empty=require_final_request_proof,
            )
            return cleanup_valid and worker_stop_confirmed and successful and final_request_proof
        return successful and worker_stop_confirmed and not alive

    if not alive:
        try:
            process.join(timeout=0)
        except Exception:
            successful = False
            worker_stop_confirmed = False
    else:
        if not _signal_health_worker(
            process,
            signal.SIGTERM,
            process_group_owned=process_group_owned,
            worker_identity=worker_identity,
        ):
            successful = False
            worker_stop_confirmed = False
        try:
            process.join(timeout=cleanup_timeout)
        except Exception:
            successful = False
            worker_stop_confirmed = False
        try:
            alive = bool(process.is_alive())
        except Exception:
            alive = True
            successful = False
            worker_stop_confirmed = False
        if alive:
            if not _signal_health_worker(
                process,
                signal.SIGKILL,
                process_group_owned=process_group_owned,
                worker_identity=worker_identity,
            ):
                successful = False
                worker_stop_confirmed = False
            try:
                process.join(timeout=cleanup_timeout)
            except Exception:
                successful = False
                worker_stop_confirmed = False
            try:
                alive = bool(process.is_alive())
            except Exception:
                alive = True
                successful = False
                worker_stop_confirmed = False

    if process_group_owned:
        boundary_confirmed = request_side_cleanup() if worker_identity is not None else False
        if not boundary_confirmed:
            successful = False
    return successful and worker_stop_confirmed and not alive


def _run_health_check_in_process(
    executor: "HealthCheckExecutor",
    registry: ManifestRegistry,
    spec: object,
    *,
    timeout_seconds: float,
    deadline: float,
    cancellation_event: Any,
    connection: Any,
    ready_connection: Any,
    launch_event: Any,
    completion_event: Any,
    reap_connection: Any = None,
) -> None:
    """Run one check after a parent-approved, private-session handshake."""

    group_owned = _establish_health_worker_group()
    try:
        try:
            ready_connection.send(_health_worker_ready_message())
        except (BrokenPipeError, EOFError, OSError, ValueError):
            return
        if not group_owned:
            return
        remaining = max(0.0, deadline - time.monotonic())
        if remaining <= 0 or not launch_event.wait(remaining):
            return
        if cancellation_event.is_set() or time.monotonic() >= deadline:
            return

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
        if time.monotonic() >= deadline and payload.status is not HealthStatus.UNKNOWN:
            payload = _payload(
                HealthStatus.UNKNOWN,
                "budget_exhausted",
                detail="Doctor run budget expired before the observation completed",
                stdout=payload.stdout,
                stderr=payload.stderr,
                output_truncated=payload.output_truncated,
            )
        try:
            connection.send(payload)
        except (BrokenPipeError, EOFError, OSError, ValueError):
            return
        try:
            # Keep the group alive until the request-side reaper has had a
            # chance to kill it.  This closes the race where a probe outlives
            # a worker that already sent its result.
            completion_event.wait(MAX_WORKER_CLEANUP_SECONDS)
        except (OSError, RuntimeError, ValueError):
            pass
    finally:
        try:
            ready_connection.close()
        except (OSError, ValueError):
            pass
        try:
            connection.close()
        except (OSError, ValueError):
            pass
        if reap_connection is not None:
            try:
                reap_connection.send({"reaped": tuple(_HEALTH_WORKER_REAPED_PROBES)})
            except (BrokenPipeError, EOFError, OSError, ValueError):
                pass
            try:
                reap_connection.close()
            except (OSError, ValueError):
                pass


def _run_health_supervisor(
    executor: "HealthCheckExecutor",
    registry: ManifestRegistry,
    spec: object,
    *,
    timeout_seconds: float,
    deadline: float,
    cancellation_event: Any,
    connection: Any,
    ready_connection: Any,
    launch_event: Any,
    completion_event: Any,
    shutdown_event: Any,
    cleanup_connection: Any,
) -> None:
    """Own a probe worker until every adopted descendant is reaped.

    This process is deliberately separate from the check worker.  The worker
    may crash or be killed while a probe is escaping its original session, but
    this subreaper remains alive long enough to adopt, signal, and reap that
    descendant before reporting cleanup to the request-side caller.
    """

    supervisor_identity: _HealthWorkerIdentity | None = None
    tracked: dict[int, int | None] = {}
    worker_reaped: set[tuple[int, int]] = set()
    inner_process: Any = None
    inner_result_parent: Any = None
    inner_result_child: Any = None
    inner_ready_parent: Any = None
    inner_ready_child: Any = None
    inner_reap_parent: Any = None
    inner_reap_child: Any = None
    inner_launch_event: Any = None
    inner_completion_event: Any = None
    cleanup_confirmed = False
    payload_sent = False

    def send_payload(payload: _CheckPayload) -> None:
        nonlocal payload_sent
        if payload_sent:
            return
        try:
            connection.send(payload)
            payload_sent = True
        except (BrokenPipeError, EOFError, OSError, ValueError):
            pass

    def sample_owned_processes() -> None:
        if supervisor_identity is None:
            return
        snapshot = _health_sample_private_processes(supervisor_identity, tracked)
        if snapshot is None:
            return
        sampled, members = snapshot
        if not sampled[1] or not members[1]:
            return
        for pid, start_time in worker_reaped:
            if tracked.get(pid) == start_time:
                tracked.pop(pid, None)

    try:
        if not _establish_health_worker_group():
            try:
                ready_connection.send(False)
            except (BrokenPipeError, EOFError, OSError, ValueError):
                pass
            send_payload(_payload(HealthStatus.UNKNOWN, "worker_unavailable", detail="supervisor setup failed"))
            return
        supervisor_identity = _HEALTH_WORKER_IDENTITY
        try:
            ready_connection.send(_health_worker_ready_message())
        except (BrokenPipeError, EOFError, OSError, ValueError):
            return
        if supervisor_identity is None:
            send_payload(_payload(HealthStatus.UNKNOWN, "worker_unavailable", detail="supervisor identity unavailable"))
            return

        remaining = max(0.0, deadline - time.monotonic())
        if remaining <= 0 or not launch_event.wait(remaining):
            send_payload(_payload(HealthStatus.UNKNOWN, "budget_exhausted", detail="launch was cancelled before the deadline"))
            return
        if shutdown_event.is_set() or cancellation_event.is_set() or time.monotonic() >= deadline:
            send_payload(_payload(HealthStatus.UNKNOWN, "budget_exhausted", detail="launch was cancelled before the deadline"))
            return

        context = multiprocessing.get_context("fork")
        inner_result_parent, inner_result_child = context.Pipe(duplex=False)
        inner_ready_parent, inner_ready_child = context.Pipe(duplex=False)
        inner_reap_parent, inner_reap_child = context.Pipe(duplex=False)
        inner_launch_event = context.Event()
        inner_completion_event = context.Event()
        inner_process = context.Process(
            target=_run_health_check_in_process,
            args=(executor, registry, spec),
            kwargs={
                "timeout_seconds": timeout_seconds,
                "deadline": deadline,
                "cancellation_event": cancellation_event,
                "connection": inner_result_child,
                "ready_connection": inner_ready_child,
                "launch_event": inner_launch_event,
                "completion_event": inner_completion_event,
                "reap_connection": inner_reap_child,
            },
        )
        inner_process.daemon = False
        inner_process.start()
        inner_result_child.close()
        inner_ready_child.close()
        inner_reap_child.close()
        inner_launch_event.set()
        # Start the supervisor-side tracker immediately.  The request-side
        # handoff is the authoritative fallback, but this closes the normal
        # worker-loss window whenever the supervisor remains alive.
        sample_owned_processes()
        next_sample_at = time.monotonic() + 0.005

        while True:
            if inner_result_parent.poll(0.005):
                try:
                    raw = inner_result_parent.recv()
                except (EOFError, OSError, ValueError):
                    raw = None
                if isinstance(raw, _CheckPayload):
                    send_payload(raw)
                else:
                    send_payload(_payload(HealthStatus.UNKNOWN, "operation_error", detail="probe worker returned no valid result"))
                break
            try:
                inner_alive = bool(inner_process.is_alive())
            except Exception:
                inner_alive = False
            if not inner_alive:
                send_payload(_payload(HealthStatus.UNKNOWN, "operation_error", detail="probe worker exited before returning a result"))
                break
            if shutdown_event.is_set() or cancellation_event.is_set() or time.monotonic() >= deadline:
                cancellation_event.set()
                send_payload(_payload(HealthStatus.UNKNOWN, "budget_exhausted", detail="probe worker exceeded the Doctor deadline"))
                break
            if inner_reap_parent.poll(0):
                try:
                    reap_message = inner_reap_parent.recv()
                except (EOFError, OSError, ValueError):
                    reap_message = None
                _health_apply_reap_notifications(tracked, worker_reaped, reap_message)
            if time.monotonic() >= next_sample_at:
                sample_owned_processes()
                next_sample_at = time.monotonic() + 0.005

        while not shutdown_event.is_set() and not completion_event.is_set():
            if time.monotonic() >= deadline:
                break
            try:
                if not inner_process.is_alive():
                    break
            except Exception:
                break
            sample_owned_processes()
            time.sleep(0.005)
    except BaseException as exc:
        send_payload(_payload(HealthStatus.UNKNOWN, "operation_error", detail=type(exc).__name__))
    finally:
        cancellation_event.set()
        if inner_completion_event is not None:
            try:
                inner_completion_event.set()
            except (OSError, RuntimeError, ValueError):
                pass
        if inner_process is not None:
            try:
                inner_process.join(timeout=min(MAX_WORKER_CLEANUP_SECONDS, 0.05))
                if not inner_process.is_alive():
                    tracked.pop(getattr(inner_process, "pid", -1), None)
            except Exception:
                pass
        if inner_reap_parent is not None:
            try:
                while inner_reap_parent.poll(0):
                    reap_message = inner_reap_parent.recv()
                    _health_apply_reap_notifications(tracked, worker_reaped, reap_message)
            except (EOFError, OSError, ValueError):
                pass
        if supervisor_identity is not None:
            try:
                sample_owned_processes()
                cleanup_confirmed = _health_cleanup_boundary(
                    supervisor_identity,
                    tracked,
                    MAX_WORKER_CLEANUP_SECONDS,
                )
            except BaseException:
                cleanup_confirmed = False
        try:
            cleanup_connection.send(
                {
                    "confirmed": cleanup_confirmed,
                    "tracked": dict(tracked),
                }
            )
        except (BrokenPipeError, EOFError, OSError, ValueError):
            cleanup_confirmed = False
        if inner_process is not None:
            try:
                inner_process.join(timeout=0)
            except Exception:
                pass
        for resource in (
            inner_result_parent,
            inner_result_child,
            inner_ready_parent,
            inner_ready_child,
            inner_reap_parent,
            inner_reap_child,
            cleanup_connection,
            ready_connection,
            connection,
        ):
            if resource is not None:
                try:
                    resource.close()
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
        ready_parent, ready_child = context.Pipe(duplex=False)
        cleanup_parent, cleanup_child = context.Pipe(duplex=False)
        cancellation_event = context.Event()
        launch_event = context.Event()
        completion_event = context.Event()
        shutdown_event = context.Event()
        request_subreaper_previous = _health_child_subreaper_state()
        request_subreaper_changed = False
        process: Any = None
        process_started = False
        process_group_owned = False
        worker_identity: _HealthWorkerIdentity | None = None
        request_identity: _HealthWorkerIdentity | None = None
        request_baseline: Mapping[int, int] | None = None
        tracked_descendants: dict[int, int | None] = {}
        cleanup_attempted = False
        cleanup_confirmed = False

        def sample_request_owned_processes() -> None:
            if (
                not process_group_owned
                or worker_identity is None
                or request_identity is None
                or request_baseline is None
            ):
                return
            if type(worker_identity.start_time) is not int:
                return
            snapshot = _health_sample_request_owned_processes(
                request_identity,
                tracked_descendants,
                request_baseline,
                excluded_identities={worker_identity.pid: worker_identity.start_time},
            )
            if snapshot is None:
                return
            _owned, _snapshot_valid = snapshot

        def cleanup_worker(*, require_final_request_proof: bool = False) -> bool:
            nonlocal cleanup_attempted, cleanup_confirmed
            if cleanup_attempted:
                return cleanup_confirmed
            cleanup_attempted = True
            if not process_started or process is None:
                cleanup_confirmed = True
                return True
            sample_request_owned_processes()
            cancellation_event.set()
            cleanup_confirmed = _reap_health_worker(
                process,
                self.worker_cleanup_seconds,
                process_group_owned=process_group_owned,
                worker_identity=worker_identity,
                tracked_descendants=tracked_descendants,
                shutdown_event=shutdown_event if process_group_owned else None,
                cleanup_connection=cleanup_parent if process_group_owned else None,
                fallback_owner_pid=os.getpid(),
                request_identity=request_identity,
                request_baseline=request_baseline,
                require_final_request_proof=require_final_request_proof,
            )
            return cleanup_confirmed

        try:
            if os.name == "posix":
                if request_subreaper_previous is None:
                    return (
                        _payload(
                            HealthStatus.UNKNOWN,
                            "worker_unavailable",
                            detail="request child-subreaper state could not be observed",
                        ),
                        max(0.0, (self.clock() - started) * 1000.0),
                    )
                if not request_subreaper_previous:
                    if not _health_prctl(_HEALTH_PR_SET_CHILD_SUBREAPER, 1):
                        return (
                            _payload(
                                HealthStatus.UNKNOWN,
                                "worker_unavailable",
                                detail="request child-subreaper boundary could not be established",
                            ),
                            max(0.0, (self.clock() - started) * 1000.0),
                        )
                    request_subreaper_changed = True
                handoff = _establish_health_request_handoff()
                if handoff is None:
                    return (
                        _payload(
                            HealthStatus.UNKNOWN,
                            "worker_unavailable",
                            detail="request descendant identity handoff could not be established",
                        ),
                        max(0.0, (self.clock() - started) * 1000.0),
                    )
                request_identity, request_baseline = handoff
            process = context.Process(
                target=_run_health_supervisor,
                args=(self, registry, spec),
                kwargs={
                    "timeout_seconds": timeout_seconds,
                    "deadline": deadline,
                    "cancellation_event": cancellation_event,
                    "connection": child_connection,
                    "ready_connection": ready_child,
                    "launch_event": launch_event,
                    "completion_event": completion_event,
                    "shutdown_event": shutdown_event,
                    "cleanup_connection": cleanup_child,
                },
            )
            process.daemon = False
            process.start()
            process_started = True
            child_connection.close()
            ready_child.close()
            cleanup_child.close()

            remaining = max(0.0, deadline - self.clock())
            raw_ready: object | None = None
            if remaining > 0 and ready_parent.poll(remaining):
                try:
                    raw_ready = ready_parent.recv()
                except (EOFError, OSError, ValueError):
                    raw_ready = None
            advertised_identity, readiness_valid = _health_ready_identity(raw_ready)
            if readiness_valid and _claim_health_worker_group(process, advertised_identity):
                worker_identity = _health_observe_worker_identity(process)
                process_group_owned = worker_identity is not None

            raw: object | None = None
            if process_group_owned and self.clock() < deadline:
                launch_event.set()
                sample_request_owned_processes()
                next_sample_at = self.clock() + 0.005
                while self.clock() < deadline:
                    remaining = max(0.0, deadline - self.clock())
                    if remaining <= 0:
                        break
                    if parent_connection.poll(min(remaining, 0.005)):
                        try:
                            raw = parent_connection.recv()
                        except (EOFError, OSError, ValueError):
                            raw = None
                        break
                    try:
                        if not process.is_alive():
                            if parent_connection.poll(0.005):
                                try:
                                    raw = parent_connection.recv()
                                except (EOFError, OSError, ValueError):
                                    raw = None
                            break
                    except Exception:
                        break
                    if self.clock() >= next_sample_at:
                        sample_request_owned_processes()
                        next_sample_at = self.clock() + 0.02
            if isinstance(raw, _CheckPayload):
                confirmed = cleanup_worker(
                    require_final_request_proof=raw.status is HealthStatus.PASS,
                )
                expired = self.clock() >= deadline
                if not process_group_owned or not confirmed:
                    if isinstance(raw, _CheckPayload) and raw.status is HealthStatus.UNKNOWN:
                        return raw, max(0.0, (self.clock() - started) * 1000.0)
                    return (
                        _payload(
                            HealthStatus.UNKNOWN,
                            "worker_cleanup_incomplete",
                            detail="default health worker cleanup could not be confirmed within its bounded envelope",
                        ),
                        max(0.0, (self.clock() - started) * 1000.0),
                    )
                if expired:
                    return (
                        _payload(
                            HealthStatus.UNKNOWN,
                            "budget_exhausted",
                            detail="Doctor run budget expired before the observation completed",
                            stdout=raw.stdout,
                            stderr=raw.stderr,
                            output_truncated=raw.output_truncated,
                        ),
                        max(0.0, (self.clock() - started) * 1000.0),
                    )
                return raw, max(0.0, (self.clock() - started) * 1000.0)

            expired = self.clock() >= deadline
            cancellation_event.set()
            confirmed = cleanup_worker()
            if not confirmed:
                code = "worker_cleanup_incomplete"
                detail = "default health worker cleanup could not be confirmed within its bounded envelope"
            elif expired:
                code = "budget_exhausted"
                detail = "bounded default health worker returned no result before the Doctor deadline"
            elif raw_ready is False:
                code = "worker_unavailable"
                detail = "default health worker could not establish a private process session"
            else:
                code = "operation_error"
                detail = "bounded default health worker returned no result"
            return (
                _payload(HealthStatus.UNKNOWN, code, detail=detail),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        except (EOFError, OSError, RuntimeError, ValueError) as exc:
            cancellation_event.set()
            confirmed = cleanup_worker()
            error_code = "operation_error" if confirmed else "worker_cleanup_incomplete"
            detail = (
                f"default health worker failed: {type(exc).__name__}"
                if confirmed
                else "default health worker cleanup could not be confirmed within its bounded envelope"
            )
            return (
                _payload(HealthStatus.UNKNOWN, error_code, detail=detail),
                max(0.0, (self.clock() - started) * 1000.0),
            )
        finally:
            cleanup_worker()
            for resource in (
                child_connection,
                ready_child,
                cleanup_child,
                parent_connection,
                ready_parent,
                cleanup_parent,
            ):
                try:
                    resource.close()
                except (OSError, ValueError):
                    pass
            if request_subreaper_changed:
                _health_prctl(
                    _HEALTH_PR_SET_CHILD_SUBREAPER,
                    1 if request_subreaper_previous else 0,
                )

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
