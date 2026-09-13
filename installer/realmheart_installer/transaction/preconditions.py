"""Mutation precondition capture and compare-before-write enforcement."""

from __future__ import annotations

from pathlib import Path

from ..errors import PreconditionFailedError
from ..filesystem.compare import fingerprint_path
from ..models import MutationPrecondition

FINGERPRINT_PRECONDITION = "path_fingerprint"


def capture_path_precondition(path: Path) -> MutationPrecondition:
    path = Path(path)
    return MutationPrecondition(
        kind=FINGERPRINT_PRECONDITION,
        expected_fingerprint=fingerprint_path(path),
        expected_exists=path.exists() or path.is_symlink(),
    )


def verify_precondition(path: Path, precondition: MutationPrecondition | None) -> None:
    if precondition is None:
        return

    path = Path(path)
    exists = path.exists() or path.is_symlink()
    if precondition.expected_exists is not None and exists != precondition.expected_exists:
        raise PreconditionFailedError(
            str(path),
            f"expected_exists={precondition.expected_exists}, current_exists={exists}",
        )

    if precondition.kind == FINGERPRINT_PRECONDITION:
        current = fingerprint_path(path)
        if current != precondition.expected_fingerprint:
            raise PreconditionFailedError(
                str(path),
                "content/type/permission fingerprint changed after planning",
            )
        return

    raise ValueError(f"Unsupported mutation precondition kind: {precondition.kind}")
