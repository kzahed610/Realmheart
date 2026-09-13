#!/usr/bin/env python3
"""Tiny Phase-20 Doctor-style consumer for Realmheart forensic state."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from realmheart_maintenance import (  # noqa: E402
    ForensicContractError,
    analyze_forensics,
    load_health_snapshot,
    load_installed_receipt,
    load_manifest,
)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Validate Realmheart's installer-to-Doctor forensic contract without importing installer code."
    )
    result.add_argument("--components", type=Path, default=REPO_ROOT / "components")
    result.add_argument("--receipt", type=Path, required=True)
    result.add_argument("--snapshot", type=Path, required=True)
    result.add_argument("--context", default="doctor_background")
    result.add_argument("--max-cost", choices=("cheap", "normal", "expensive"), default="cheap")
    result.add_argument("--json", action="store_true", dest="as_json")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        registry = load_manifest(args.components)
        receipt = load_installed_receipt(args.receipt)
        snapshot = load_health_snapshot(args.snapshot)
        report = analyze_forensics(
            registry,
            receipt,
            snapshot,
            health_context=args.context,
            max_health_cost=args.max_cost,
        )
    except ForensicContractError as exc:
        print(f"Forensic contract error: {exc}", file=sys.stderr)
        return 30
    except ValueError as exc:
        print(f"Manifest/forensic input error: {exc}", file=sys.stderr)
        return 30

    if args.as_json:
        print(json.dumps(report.to_dict(), indent=2, sort_keys=True))
        return 0

    print("Realmheart Installer ↔ Doctor forensic contract")
    print(f"  Installed release .... {report.realmheart_version}")
    print(f"  Receipt transaction .. {report.receipt_transaction_id}")
    print(f"  Runtime health ....... {report.runtime_health}")
    print(f"  Repair readiness ..... {report.repair_readiness.value}")
    print(f"  Health checks ........ {len(report.selected_health_check_ids)} selected")
    print(f"  Drift records ........ {len(report.drifts)}")
    print(f"  Root incidents ....... {len(report.incidents)}")
    if report.receipt_manifest_digest != report.manifest_digest:
        print("  Manifest identity .... DRIFTED")
    else:
        print("  Manifest identity .... MATCH")
    if report.incidents:
        print("\nIncidents")
        for incident in report.incidents:
            affected = ", ".join(incident.affected_components) or "none"
            print(f"  {incident.error_code} [{incident.severity}] {incident.root_id}")
            print(f"    affected: {affected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
