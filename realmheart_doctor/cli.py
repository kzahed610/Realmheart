"""Realmheart Doctor Acceptance MVP command line."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from realmheart_maintenance.forensics import ForensicContractError
from realmheart_maintenance.manifest import ManifestError, load_manifest

from .acceptance import DoctorAcceptanceError, assess_candidate_install, load_candidate_bundle


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="realmheart-doctor", description="Realmheart read-only health acceptance assessor")
    parser.add_argument("--version", action="version", version="realmheart-doctor 0.7.8")
    sub = parser.add_subparsers(dest="command", required=True)
    assess = sub.add_parser("assess-install", help="independently assess a candidate Realmheart installation")
    assess.add_argument("--candidate", type=Path, required=True)
    assess.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    assess.add_argument("--json", action="store_true")
    return parser

from .render import render_acceptance_assessment


def _render_expected_error(args: argparse.Namespace, exc: Exception) -> None:
    message = str(exc)
    if getattr(args, "json", False):
        print(json.dumps({"recommendation": "indeterminate", "error": message}, indent=2, sort_keys=True))
    else:
        print(f"Realmheart Doctor could not assess the candidate: {message}")


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        registry = load_manifest(args.manifest_dir)
        assessment = assess_candidate_install(registry, load_candidate_bundle(args.candidate))
    except (DoctorAcceptanceError, ForensicContractError, ManifestError, OSError) as exc:
        _render_expected_error(args, exc)
        return 3
    if args.json:
        print(json.dumps(assessment.to_dict(), indent=2, sort_keys=True))
    else:
        print(render_acceptance_assessment(assessment))
    if assessment.recommendation.value == "revert_recommended":
        return 2
    if assessment.recommendation.value == "indeterminate":
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
