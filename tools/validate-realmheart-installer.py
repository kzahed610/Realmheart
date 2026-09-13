#!/usr/bin/env python3
"""Realmheart Phase-19 real-system validation matrix runner."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
INSTALLER_ROOT = REPO_ROOT / "installer"
if str(INSTALLER_ROOT) not in sys.path:
    sys.path.insert(0, str(INSTALLER_ROOT))

from realmheart_installer.validation.phase19 import (  # noqa: E402
    PHASE19_SCENARIOS,
    ValidationStatus,
    create_report,
    load_report,
    record_result,
    run_fixture_matrix,
    run_host_audit,
    save_report,
    summarize_report,
)


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="validate-realmheart-installer",
        description="Phase-19 Realmheart installer validation matrix/evidence ledger",
    )
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="list the canonical Phase-19 scenarios")

    guide = sub.add_parser("guide", help="show setup/acceptance criteria for one real-system scenario")
    guide.add_argument("scenario", choices=[item.scenario_id for item in PHASE19_SCENARIOS])

    init = sub.add_parser("init", help="create a new private Phase-19 evidence ledger")
    init.add_argument("--report", type=Path, required=True)

    fixture = sub.add_parser("fixture-matrix", help="run the isolated fixture counterpart for every Phase-19 scenario")
    fixture.add_argument("--report", type=Path, required=True)
    fixture.add_argument("--timeout", type=int, default=180)
    fixture.add_argument("--evidence-dir", type=Path, help="store private mode-0600 fixture output for debugging/evidence")

    audit = sub.add_parser("host-audit", help="run read-only host readiness checks and store private evidence logs")
    audit.add_argument("--report", type=Path, required=True)
    audit.add_argument("--evidence-dir", type=Path, required=True)
    audit.add_argument("--timeout", type=int, default=180)

    record = sub.add_parser("record", help="record the result of one destructive/disposable-host scenario")
    record.add_argument("--report", type=Path, required=True)
    record.add_argument("--scenario", required=True, choices=[item.scenario_id for item in PHASE19_SCENARIOS])
    record.add_argument("--status", required=True, choices=[item.value for item in ValidationStatus if item is not ValidationStatus.PENDING])
    record.add_argument("--note")
    record.add_argument("--evidence", action="append", type=Path, default=[])
    record.add_argument("--evidence-dir", type=Path)
    record.add_argument("--validated-source-root", type=Path, default=REPO_ROOT, help="source checkout actually exercised for this scenario")

    summary = sub.add_parser("summary", help="show fixture/host/live matrix status; success only when the whole matrix is PASS")
    summary.add_argument("--report", type=Path, required=True)
    summary.add_argument("--json", action="store_true")
    return p


def _scenario(sid: str):
    return next(item for item in PHASE19_SCENARIOS if item.scenario_id == sid)


def _print_summary(report) -> int:
    summary = summarize_report(report)
    print("Realmheart Installer — Phase 19 validation")
    print(f"  Scenarios ............ {summary['scenario_count']}")
    print(f"  Fixture matrix ....... {summary['fixture']['pass']}/{summary['scenario_count']} PASS")
    print(f"  Host audit ........... {'PASS' if summary['host_audit_pass'] else 'INCOMPLETE/FAIL'}")
    print(f"  Real-system matrix ... {summary['live']['pass']}/{summary['scenario_count']} PASS")
    outstanding = [sid for sid, item in report.scenarios.items() if item.status is not ValidationStatus.PASS]
    if outstanding:
        print("  Outstanding .......... " + ", ".join(outstanding))
    print(f"  Phase 19 ............. {'COMPLETE' if summary['phase19_complete'] else 'INCOMPLETE'}")
    return 0 if summary["phase19_complete"] else 1


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if args.command == "list":
        print("Realmheart Installer — Phase 19 scenarios")
        for index, item in enumerate(PHASE19_SCENARIOS, 1):
            print(f"  [{index:02d}] {item.scenario_id:<24} {item.title}")
        print("\nDestructive live validation belongs on disposable users/VM snapshots only.")
        return 0

    if args.command == "guide":
        item = _scenario(args.scenario)
        print(f"{item.title} ({item.scenario_id})")
        print(f"\nPurpose\n  {item.purpose}")
        print("\nSetup / execution")
        for step in item.setup:
            print(f"  - {step}")
        print("\nAcceptance")
        for criterion in item.acceptance:
            print(f"  - {criterion}")
        print("\nFixture counterparts")
        for test in item.fixture_tests:
            print(f"  - {test}")
        return 0

    if args.command == "init":
        if args.report.exists():
            print(f"Refusing to overwrite existing report: {args.report}", file=sys.stderr)
            return 2
        report = create_report(REPO_ROOT)
        save_report(report, args.report)
        print(f"Created Phase-19 validation ledger: {args.report}")
        return 0

    report = load_report(args.report)
    report_root = Path(report.source_root).resolve()
    if report_root != REPO_ROOT.resolve():
        print(f"Warning: ledger source root is {report_root}; current source root is {REPO_ROOT.resolve()}", file=sys.stderr)

    if args.command == "fixture-matrix":
        results = run_fixture_matrix(report, source_root=REPO_ROOT, timeout=args.timeout, evidence_dir=args.evidence_dir)
        save_report(report, args.report)
        print("Realmheart Installer — Phase 19 fixture matrix")
        for sid, status, _duration in results:
            print(f"  {sid:<24} {status.value.upper():<5}")
        passed = sum(1 for _, status, _ in results if status is ValidationStatus.PASS)
        corpus_duration = results[0][2] if results else 0.0
        print(f"\nFixture corpus: {corpus_duration:.3f}s")
        print(f"Result: {passed}/{len(results)} scenarios PASS")
        return 0 if passed == len(results) else 1

    if args.command == "host-audit":
        passed = run_host_audit(report, source_root=REPO_ROOT, evidence_dir=args.evidence_dir, timeout=args.timeout)
        save_report(report, args.report)
        print("Realmheart Installer — Phase 19 host audit")
        for name, item in report.host_audit.get("checks", {}).items():
            print(f"  {name:<20} {str(item['status']).upper():<5} rc={item['returncode']} {item['duration_seconds']}s")
        print(f"\nResult: {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1

    if args.command == "record":
        result = record_result(
            report,
            scenario_id=args.scenario,
            status=ValidationStatus(args.status),
            note=args.note,
            evidence_paths=args.evidence,
            evidence_dir=args.evidence_dir,
            validated_source_root=args.validated_source_root,
        )
        save_report(report, args.report)
        print(f"Recorded {result.scenario_id}: {result.status.value.upper()}")
        if result.evidence_files:
            print(f"Evidence files: {len(result.evidence_files)}")
        return 0 if result.status is ValidationStatus.PASS else 1

    if args.command == "summary":
        if args.json:
            print(json.dumps(summarize_report(report), indent=2, sort_keys=True))
            return 0 if summarize_report(report)["phase19_complete"] else 1
        return _print_summary(report)

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
