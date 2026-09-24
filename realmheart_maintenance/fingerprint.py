"""Stable cross-tool filesystem fingerprinting for Realmheart maintenance state."""
from __future__ import annotations

import hashlib
import math
import os
import stat
import time
from dataclasses import dataclass
from pathlib import Path

_CHUNK_SIZE = 1024 * 1024
MAX_FINGERPRINT_ENTRIES = 100_000
MAX_FINGERPRINT_DEPTH = 64
MAX_FINGERPRINT_BYTES = 128 * 1024 * 1024
MAX_SHA256_BYTES = MAX_FINGERPRINT_BYTES
MAX_FINGERPRINT_SECONDS = 30.0


class FingerprintObservationError(OSError):
    """Raised when a descriptor-bound observation cannot be trusted."""

    def __init__(self, detail: str, *, resource: str | None = None, reason: str = "unknown") -> None:
        self.resource = resource
        self.reason = reason
        super().__init__(detail)


class DescriptorSafetyError(FingerprintObservationError):
    """Raised when the platform cannot provide the required descriptor safety."""


@dataclass(frozen=True)
class PathObservation:
    """One descriptor-bound observation of a path.

    ``sha256`` and ``immutable_fingerprint`` are populated only when requested.
    A missing path is represented explicitly instead of being converted into a
    digest that could be mistaken for a clean observation.
    """

    exists: bool
    mode: int | None
    filesystem_type: str | None
    sha256: str | None = None
    immutable_fingerprint: str | None = None


def _open(path, flags: int, *, dir_fd: int | None = None) -> int:
    """Filesystem seam used by deterministic descriptor-race tests."""

    if dir_fd is None:
        return os.open(path, flags)
    return os.open(path, flags, dir_fd=dir_fd)


def _fstat(descriptor: int) -> os.stat_result:
    """Filesystem seam used by deterministic descriptor-race tests."""

    return os.fstat(descriptor)


def _lstat(path, *, dir_fd: int | None = None) -> os.stat_result:
    """Filesystem seam used by deterministic path-identity tests."""

    if dir_fd is None:
        return os.lstat(path)
    try:
        return os.stat(path, dir_fd=dir_fd, follow_symlinks=False)
    except (NotImplementedError, TypeError) as exc:
        raise DescriptorSafetyError(
            "descriptor-relative identity checks are unavailable on this platform",
            reason="unsupported",
        ) from exc


def _read(descriptor: int, size: int) -> bytes:
    return os.read(descriptor, size)


def _close(descriptor: int) -> None:
    os.close(descriptor)


def _scandir(descriptor: int):
    """Return a descriptor-relative iterator without reopening its pathname."""

    return os.scandir(descriptor)


def _readlink(path, *, dir_fd: int | None = None) -> str:
    if dir_fd is None:
        return os.readlink(path)
    try:
        return os.readlink(path, dir_fd=dir_fd)
    except (NotImplementedError, TypeError) as exc:
        raise DescriptorSafetyError(
            "descriptor-relative symlink reads are unavailable on this platform",
            reason="unsupported",
        ) from exc


class FingerprintLimitExceeded(FingerprintObservationError):
    """Raised when bounded fingerprint traversal would exceed its budget.

    Limit exhaustion is an observation failure, not an unexpected runtime crash.
    Callers that translate ``OSError``/``FingerprintObservationError`` can
    therefore report a stable, structured diagnostic.
    """

    def __init__(self, resource: str, limit: int | float, observed: int | float) -> None:
        self.limit_resource = resource
        self.limit = limit
        self.observed = observed
        super().__init__(
            f"fingerprint {resource} limit exceeded: {observed} > {limit}",
            resource=resource,
            reason="limit_exceeded",
        )


def _feed_field(hasher, name: str, value: str | bytes) -> None:
    raw = value.encode("utf-8", errors="surrogateescape") if isinstance(value, str) else value
    hasher.update(name.encode("ascii"))
    hasher.update(b"\0")
    hasher.update(str(len(raw)).encode("ascii"))
    hasher.update(b":")
    hasher.update(raw)
    hasher.update(b"\0")


def _feed_stat(hasher, st: os.stat_result) -> None:
    _feed_field(hasher, "mode", oct(stat.S_IMODE(st.st_mode)))


def _validate_limit(name: str, value: int, hard_limit: int) -> int:
    if type(value) is not int or value < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    if value > hard_limit:
        raise ValueError(f"{name} exceeds hard limit {hard_limit}")
    return value


def _filesystem_type(mode: int) -> str:
    if stat.S_ISREG(mode):
        return "file"
    if stat.S_ISDIR(mode):
        return "directory"
    if stat.S_ISLNK(mode):
        return "symlink"
    return "other"


def _stat_identity(value: os.stat_result) -> tuple[int, int, int]:
    return (value.st_dev, value.st_ino, stat.S_IFMT(value.st_mode))


def _stat_metadata(value: os.stat_result) -> tuple[object, ...]:
    """Metadata that must remain stable during one bound observation.

    Access time is intentionally excluded: opening and reading a file can
    update it without changing the observed content or the bound inode.
    """

    return (
        _stat_identity(value),
        value.st_mode,
        value.st_nlink,
        value.st_size,
        getattr(value, "st_mtime_ns", value.st_mtime),
        getattr(value, "st_ctime_ns", value.st_ctime),
    )


def _assert_stable_stat(
    expected: os.stat_result,
    actual: os.stat_result,
    *,
    resource: str,
    phase: str,
) -> None:
    if _stat_identity(expected) != _stat_identity(actual):
        raise FingerprintObservationError(
            f"{resource} identity/type changed {phase}",
            resource=resource,
            reason="identity_mismatch",
        )
    if _stat_metadata(expected) != _stat_metadata(actual):
        raise FingerprintObservationError(
            f"{resource} metadata changed {phase}",
            resource=resource,
            reason="metadata_mismatch",
        )


def _assert_path_stable(path: Path, expected: os.stat_result, *, phase: str) -> None:
    try:
        actual = _lstat(path)
    except OSError as exc:
        raise FingerprintObservationError(
            f"{path} could not be revalidated {phase}: {exc}",
            resource=os.fspath(path),
            reason="path_changed",
        ) from exc
    _assert_stable_stat(expected, actual, resource=os.fspath(path), phase=phase)


def _assert_child_stable(
    directory: int,
    name: str,
    expected: os.stat_result,
    *,
    phase: str,
) -> None:
    try:
        actual = _lstat(name, dir_fd=directory)
    except OSError as exc:
        raise FingerprintObservationError(
            f"directory entry {name!r} could not be revalidated {phase}: {exc}",
            resource=name,
            reason="path_changed",
        ) from exc
    _assert_stable_stat(expected, actual, resource=name, phase=phase)


def _descriptor_flags(*, directory: bool) -> int:
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nofollow is None:
        raise DescriptorSafetyError(
            "descriptor-safe no-follow open is unavailable on this platform",
            reason="unsupported",
        )
    flags = os.O_RDONLY | nofollow
    cloexec = getattr(os, "O_CLOEXEC", None)
    if cloexec is not None:
        flags |= cloexec
    if not directory:
        nonblocking = getattr(os, "O_NONBLOCK", None)
        if nonblocking is None:
            raise DescriptorSafetyError(
                "descriptor-safe non-blocking open is unavailable on this platform",
                reason="unsupported",
            )
        # A regular file normally ignores this flag.  If the pathname is
        # replaced with a FIFO before open, it prevents the safety check below
        # from blocking before fstat can reject the special file.
        flags |= nonblocking
    if directory:
        directory_flag = getattr(os, "O_DIRECTORY", None)
        if directory_flag is None:
            raise DescriptorSafetyError(
                "descriptor-relative directory open is unavailable on this platform",
                reason="unsupported",
            )
        flags |= directory_flag
    return flags


def _open_descriptor(
    path,
    *,
    directory: bool = False,
    dir_fd: int | None = None,
) -> int:
    flags = _descriptor_flags(directory=directory)
    try:
        return _open(path, flags, dir_fd=dir_fd)
    except (NotImplementedError, TypeError) as exc:
        raise DescriptorSafetyError(
            "descriptor-relative open is unavailable on this platform",
            reason="unsupported",
        ) from exc
    except OSError as exc:
        raise FingerprintObservationError(
            f"descriptor open failed for {path!r}: {exc}",
            resource=os.fspath(path),
            reason="open_failed",
        ) from exc


def _check_deadline(deadline: float, *, started_at: float, seconds_limit: float) -> None:
    now = time.monotonic()
    if now > deadline:
        raise FingerprintLimitExceeded("seconds", seconds_limit, now - started_at)


def _read_regular_descriptor(
    descriptor: int,
    *,
    max_bytes: int | None,
    used_bytes: int = 0,
    deadline: float | None = None,
    started_at: float | None = None,
    seconds_limit: float | None = None,
    fingerprint_hasher=None,
    sha256_hasher=None,
    limit_resource: str = "bytes",
) -> int:
    while True:
        if deadline is not None and started_at is not None and seconds_limit is not None:
            _check_deadline(deadline, started_at=started_at, seconds_limit=seconds_limit)
        remaining = None if max_bytes is None else max_bytes - used_bytes
        try:
            chunk = _read(
                descriptor,
                _CHUNK_SIZE if remaining is None else min(_CHUNK_SIZE, remaining + 1),
            )
        except InterruptedError:
            continue
        if not chunk:
            break
        if remaining is not None and len(chunk) > remaining:
            raise FingerprintLimitExceeded(limit_resource, max_bytes, used_bytes + len(chunk))
        if fingerprint_hasher is not None:
            fingerprint_hasher.update(chunk)
        if sha256_hasher is not None:
            sha256_hasher.update(chunk)
        used_bytes += len(chunk)
    return used_bytes


def _directory_names(
    directory: int,
    *,
    max_entries: int,
    entries_seen: int,
    count_entries: bool,
) -> tuple[list[str], int]:
    try:
        iterator = _scandir(directory)
    except (AttributeError, NotImplementedError, TypeError) as exc:
        raise DescriptorSafetyError(
            "descriptor-relative directory iteration is unavailable on this platform",
            reason="unsupported",
        ) from exc
    names: list[str] = []
    try:
        with iterator:
            for entry in iterator:
                name = os.fsdecode(entry.name)
                names.append(name)
                if count_entries:
                    entries_seen += 1
                    if entries_seen > max_entries:
                        raise FingerprintLimitExceeded("entries", max_entries, entries_seen)
                elif len(names) > max_entries:
                    raise FingerprintLimitExceeded("entries", max_entries, len(names))
    except FingerprintLimitExceeded:
        raise
    except OSError as exc:
        raise FingerprintObservationError(
            f"descriptor-relative directory enumeration failed: {exc}",
            reason="directory_read_failed",
        ) from exc
    return names, entries_seen


@dataclass
class _TraversalState:
    entries_seen: int = 0
    used_bytes: int = 0


def _fingerprint_directory_descriptor(
    hasher,
    directory: int,
    *,
    relative_prefix: tuple[str, ...],
    depth: int,
    max_entries: int,
    max_depth: int,
    max_bytes: int | None,
    deadline: float,
    started_at: float,
    seconds_limit: float,
    state: _TraversalState,
) -> None:
    _check_deadline(deadline, started_at=started_at, seconds_limit=seconds_limit)
    before = _fstat(directory)
    names, state.entries_seen = _directory_names(
        directory,
        max_entries=max_entries,
        entries_seen=state.entries_seen,
        count_entries=True,
    )
    names.sort(key=os.fsencode)
    for name in names:
        _check_deadline(deadline, started_at=started_at, seconds_limit=seconds_limit)
        try:
            entry_stat = _lstat(name, dir_fd=directory)
        except OSError as exc:
            raise FingerprintObservationError(
                f"directory entry {name!r} could not be observed: {exc}",
                resource=name,
                reason="entry_changed",
            ) from exc
        relative = relative_prefix + (name,)
        _feed_field(hasher, "path", "/".join(relative))
        _feed_stat(hasher, entry_stat)
        if stat.S_ISLNK(entry_stat.st_mode):
            _feed_field(hasher, "type", "symlink")
            try:
                target = _readlink(name, dir_fd=directory)
            except OSError as exc:
                raise FingerprintObservationError(
                    f"symlink {name!r} could not be observed: {exc}",
                    resource=name,
                    reason="entry_changed",
                ) from exc
            _feed_field(hasher, "target", target)
            _assert_child_stable(directory, name, entry_stat, phase="after symlink read")
            continue
        if stat.S_ISREG(entry_stat.st_mode):
            _feed_field(hasher, "type", "file")
            descriptor = _open_descriptor(name, dir_fd=directory)
            try:
                opened = _fstat(descriptor)
                _assert_stable_stat(entry_stat, opened, resource=name, phase="before file read")
                state.used_bytes = _read_regular_descriptor(
                    descriptor,
                    max_bytes=max_bytes,
                    used_bytes=state.used_bytes,
                    deadline=deadline,
                    started_at=started_at,
                    seconds_limit=seconds_limit,
                    fingerprint_hasher=hasher,
                )
                _assert_stable_stat(entry_stat, _fstat(descriptor), resource=name, phase="after file read")
                _assert_child_stable(directory, name, entry_stat, phase="after file read")
            finally:
                _close(descriptor)
            continue
        if stat.S_ISDIR(entry_stat.st_mode):
            _feed_field(hasher, "type", "directory")
            if depth >= max_depth:
                raise FingerprintLimitExceeded("depth", max_depth, depth + 1)
            descriptor = _open_descriptor(name, directory=True, dir_fd=directory)
            try:
                opened = _fstat(descriptor)
                _assert_stable_stat(entry_stat, opened, resource=name, phase="before directory read")
                _fingerprint_directory_descriptor(
                    hasher,
                    descriptor,
                    relative_prefix=relative,
                    depth=depth + 1,
                    max_entries=max_entries,
                    max_depth=max_depth,
                    max_bytes=max_bytes,
                    deadline=deadline,
                    started_at=started_at,
                    seconds_limit=seconds_limit,
                    state=state,
                )
                _assert_stable_stat(entry_stat, _fstat(descriptor), resource=name, phase="after directory read")
                _assert_child_stable(directory, name, entry_stat, phase="after directory read")
            finally:
                _close(descriptor)
            continue
        _feed_field(hasher, "type", f"special:{stat.S_IFMT(entry_stat.st_mode)}")
        _assert_child_stable(directory, name, entry_stat, phase="after special-file read")

    after = _fstat(directory)
    _assert_stable_stat(before, after, resource=f"directory fd {directory}", phase="after directory read")
    after_names, _ = _directory_names(
        directory,
        max_entries=max_entries,
        entries_seen=state.entries_seen,
        count_entries=False,
    )
    after_names.sort(key=os.fsencode)
    if names != after_names:
        raise FingerprintObservationError(
            "directory entries changed during descriptor-bound observation",
            resource=f"directory fd {directory}",
            reason="directory_changed",
        )


def _validate_fingerprint_options(
    *,
    max_entries: int,
    max_depth: int,
    max_bytes: int | None,
    max_seconds: int | float,
) -> float:
    _validate_limit("max_entries", max_entries, MAX_FINGERPRINT_ENTRIES)
    _validate_limit("max_depth", max_depth, MAX_FINGERPRINT_DEPTH)
    if max_bytes is not None:
        _validate_limit("max_bytes", max_bytes, MAX_FINGERPRINT_BYTES)
    if (
        isinstance(max_seconds, bool)
        or not isinstance(max_seconds, (int, float))
        or not math.isfinite(max_seconds)
        or max_seconds < 0
    ):
        raise ValueError("max_seconds must be a finite non-negative number")
    if max_seconds > MAX_FINGERPRINT_SECONDS:
        raise ValueError(f"max_seconds exceeds hard limit {MAX_FINGERPRINT_SECONDS:g}")
    return float(max_seconds)


def observe_path(
    path: Path,
    *,
    include_sha256: bool = False,
    include_fingerprint: bool = False,
    initial_stat: os.stat_result | None = None,
    max_entries: int = MAX_FINGERPRINT_ENTRIES,
    max_depth: int = MAX_FINGERPRINT_DEPTH,
    max_bytes: int | None = MAX_FINGERPRINT_BYTES,
    max_seconds: int | float = MAX_FINGERPRINT_SECONDS,
) -> PathObservation:
    """Observe a path through one bound descriptor where content is required.

    The optional ``initial_stat`` lets a caller reuse its already-authorized
    ``lstat`` result.  Every regular-file and directory read then performs
    descriptor ``fstat`` checks before/after the read plus a post-read path
    identity check.  The module-level filesystem wrappers above are an
    intentional deterministic test seam: tests can inject an identity change
    without relying on a timing race.
    """

    seconds_limit = _validate_fingerprint_options(
        max_entries=max_entries,
        max_depth=max_depth,
        max_bytes=max_bytes,
        max_seconds=max_seconds,
    )
    path = Path(path)
    started_at = time.monotonic()
    deadline = started_at + seconds_limit
    _check_deadline(deadline, started_at=started_at, seconds_limit=seconds_limit)
    if initial_stat is None:
        try:
            initial_stat = _lstat(path)
        except FileNotFoundError:
            missing_hash = None
            if include_fingerprint:
                missing_hasher = hashlib.sha256()
                _feed_field(missing_hasher, "root", "missing")
                missing_hash = missing_hasher.hexdigest()
            return PathObservation(False, None, None, immutable_fingerprint=missing_hash)

    filesystem_type = _filesystem_type(initial_stat.st_mode)
    mode = stat.S_IMODE(initial_stat.st_mode)
    if filesystem_type == "symlink":
        symlink_hash = None
        if include_fingerprint:
            symlink_hasher = hashlib.sha256()
            _feed_field(symlink_hasher, "root-type", "symlink")
            _feed_stat(symlink_hasher, initial_stat)
            try:
                target = _readlink(path)
            except OSError as exc:
                raise FingerprintObservationError(
                    f"symlink target could not be observed: {exc}",
                    resource=os.fspath(path),
                    reason="path_changed",
                ) from exc
            _feed_field(symlink_hasher, "target", target)
            symlink_hash = symlink_hasher.hexdigest()
        _assert_path_stable(path, initial_stat, phase="after symlink observation")
        return PathObservation(True, mode, filesystem_type, immutable_fingerprint=symlink_hash)

    if filesystem_type == "other":
        special_hash = None
        if include_fingerprint:
            special_hasher = hashlib.sha256()
            _feed_field(special_hasher, "root-type", f"special:{stat.S_IFMT(initial_stat.st_mode)}")
            _feed_stat(special_hasher, initial_stat)
            special_hash = special_hasher.hexdigest()
        _assert_path_stable(path, initial_stat, phase="after special-file observation")
        return PathObservation(True, mode, filesystem_type, immutable_fingerprint=special_hash)

    descriptor = _open_descriptor(path, directory=filesystem_type == "directory")
    try:
        opened = _fstat(descriptor)
        _assert_stable_stat(initial_stat, opened, resource=os.fspath(path), phase="before read")
        fingerprint_hasher = hashlib.sha256() if include_fingerprint else None
        sha256_hasher = hashlib.sha256() if include_sha256 else None
        if fingerprint_hasher is not None:
            _feed_field(
                fingerprint_hasher,
                "root-type",
                "directory" if filesystem_type == "directory" else "file",
            )
            _feed_stat(fingerprint_hasher, initial_stat)
        if filesystem_type == "file" and (
            fingerprint_hasher is not None or sha256_hasher is not None
        ):
            _read_regular_descriptor(
                descriptor,
                max_bytes=max_bytes,
                deadline=deadline,
                started_at=started_at,
                seconds_limit=seconds_limit,
                fingerprint_hasher=fingerprint_hasher,
                sha256_hasher=sha256_hasher,
                limit_resource="bytes",
            )
        elif fingerprint_hasher is not None:
            _fingerprint_directory_descriptor(
                fingerprint_hasher,
                descriptor,
                relative_prefix=(),
                depth=0,
                max_entries=max_entries,
                max_depth=max_depth,
                max_bytes=max_bytes,
                deadline=deadline,
                started_at=started_at,
                seconds_limit=seconds_limit,
                state=_TraversalState(),
            )
        _assert_stable_stat(initial_stat, _fstat(descriptor), resource=os.fspath(path), phase="after read")
        _assert_path_stable(path, initial_stat, phase="after read")
    finally:
        _close(descriptor)
    return PathObservation(
        True,
        mode,
        filesystem_type,
        sha256=sha256_hasher.hexdigest() if sha256_hasher is not None else None,
        immutable_fingerprint=(
            fingerprint_hasher.hexdigest() if fingerprint_hasher is not None else None
        ),
    )


def read_regular_file(
    path: Path,
    *,
    max_bytes: int,
    hard_limit: int = MAX_FINGERPRINT_BYTES,
    initial_stat: os.stat_result | None = None,
) -> bytes:
    """Read one regular file through an identity-checked descriptor."""

    _validate_limit("max_bytes", max_bytes, hard_limit)
    path = Path(path)
    if initial_stat is None:
        initial_stat = _lstat(path)
    if stat.S_ISLNK(initial_stat.st_mode):
        raise FingerprintObservationError(
            f"{path} must not be a symlink",
            resource=os.fspath(path),
            reason="symlink",
        )
    if not stat.S_ISREG(initial_stat.st_mode):
        raise FingerprintObservationError(
            f"{path} is not a regular file",
            resource=os.fspath(path),
            reason="wrong_type",
        )
    descriptor = _open_descriptor(path)
    try:
        opened = _fstat(descriptor)
        _assert_stable_stat(initial_stat, opened, resource=os.fspath(path), phase="before read")
        chunks: list[bytes] = []
        total = 0
        while True:
            remaining = max_bytes - total
            try:
                chunk = _read(descriptor, min(_CHUNK_SIZE, remaining + 1))
            except InterruptedError:
                continue
            if not chunk:
                break
            if len(chunk) > remaining:
                raise FingerprintLimitExceeded("bytes", max_bytes, total + len(chunk))
            chunks.append(chunk)
            total += len(chunk)
        _assert_stable_stat(initial_stat, _fstat(descriptor), resource=os.fspath(path), phase="after read")
        _assert_path_stable(path, initial_stat, phase="after read")
        return b"".join(chunks)
    finally:
        _close(descriptor)


def fingerprint_path(
    path: Path,
    *,
    max_entries: int = MAX_FINGERPRINT_ENTRIES,
    max_depth: int = MAX_FINGERPRINT_DEPTH,
    max_bytes: int | None = MAX_FINGERPRINT_BYTES,
    max_seconds: int | float = MAX_FINGERPRINT_SECONDS,
) -> str:
    """Fingerprint content/shape/mode without following symlinks.

    ``max_entries`` counts descendants of a directory, ``max_depth`` counts
    directory levels below the root, and a numeric ``max_bytes`` bounds all
    regular-file content read during the traversal.  ``max_bytes=None`` keeps
    exact streaming fingerprints while relying on the entry/depth/deadline
    budgets instead of imposing a total-content byte ceiling.  Regular files and directories are
    opened once and observed through descriptor-relative operations; a path or
    inode change raises ``FingerprintObservationError`` instead of producing a
    clean digest.  The path-only call remains backward compatible.
    """

    observation = observe_path(
        path,
        include_fingerprint=True,
        max_entries=max_entries,
        max_depth=max_depth,
        max_bytes=max_bytes,
        max_seconds=max_seconds,
    )
    if observation.immutable_fingerprint is None:
        raise FingerprintObservationError(
            f"fingerprint was not produced for {path}",
            resource=os.fspath(path),
            reason="incomplete",
        )
    return observation.immutable_fingerprint


def fingerprint_regular_bytes(content: bytes, mode: int) -> str:
    hasher = hashlib.sha256()
    _feed_field(hasher, "root-type", "file")
    _feed_field(hasher, "mode", oct(stat.S_IMODE(mode)))
    hasher.update(content)
    return hasher.hexdigest()
