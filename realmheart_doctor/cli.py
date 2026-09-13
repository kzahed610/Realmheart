"""Realmheart Doctor Acceptance MVP command line."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

from realmheart_maintenance.manifest import load_manifest

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


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        registry = load_manifest(args.manifest_dir)
        assessment = assess_candidate_install(registry, load_candidate_bundle(args.candidate))
    except DoctorAcceptanceError as exc:
        if getattr(args, "json", False):
            print(json.dumps({"recommendation": "indeterminate", "error": str(exc)}, indent=2, sort_keys=True))
        else:
            print(f"Realmheart Doctor could not assess the candidate: {exc}")
        return 3
    if args.json:
        print(json.dumps(assessment.to_dict(), indent=2, sort_keys=True))
    else:
        print(render_acceptance_assessment(assessment))
    return 2 if assessment.recommendation.value == "revert_recommended" else 0


if __name__ == "__main__":
    raise SystemExit(main())
