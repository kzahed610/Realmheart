"""Realmheart Doctor Acceptance MVP command line."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
import sys
from pathlib import Path
from typing import NoReturn

from realmheart_maintenance.forensics import ForensicContractError
from realmheart_maintenance.manifest import ManifestError, load_manifest
from realmheart_maintenance.forensics import load_installed_receipt
from realmheart_maintenance.version import RELEASE_VERSION

from .acceptance import DoctorAcceptanceError, assess_candidate_install, load_candidate_bundle


def _absolute_path(value: str) -> str:
    if not Path(value).is_absolute() or "\x00" in value or "$" in value or ".." in Path(value).parts:
        raise argparse.ArgumentTypeError("expected an absolute installation directory")
    return value


def _receipt_path(value: str) -> Path:
    path = Path(value)
    if not path.is_file():
        raise argparse.ArgumentTypeError("receipt must be an existing installed-state.json file")
    return path


@contextmanager
def _installation_environment(args: argparse.Namespace):
    overrides = {name.upper(): getattr(args, name) for name in ("prefix", "libexec", "sysconf")
                 if getattr(args, name) is not None}
    if "PREFIX" in overrides and "LIBEXEC" not in overrides:
        overrides["LIBEXEC"] = str(Path(overrides["PREFIX"]) / "libexec")
    previous = {name: os.environ.get(name) for name in overrides}
    try:
        os.environ.update(overrides)
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


class _InvalidInvocation(ValueError):
    pass


class _ManualParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        raise _InvalidInvocation("invalid_invocation")


def _parser(*, manual: bool = False) -> argparse.ArgumentParser:
    parser_type = _ManualParser if manual else argparse.ArgumentParser
    parser = parser_type(prog="realmheart-doctor", description="Realmheart read-only health diagnosis and acceptance")
    parser.add_argument("--version", action="version", version=f"realmheart-doctor {RELEASE_VERSION}")
    sub = parser.add_subparsers(dest="command", required=True)
    incident = sub.add_parser("incident", help="preview or export a saved incident without new probes")
    incident.add_argument("incident_id")
    incident.add_argument("--report", action="store_true")
    incident.add_argument("--state-dir", type=Path, required=True)
    incident.add_argument("--json", action="store_true")
    incident.add_argument("--preview", action="store_true")
    assess = sub.add_parser("assess-install", help="independently assess a candidate Realmheart installation")
    assess.add_argument("--candidate", type=Path, required=True)
    assess.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    assess.add_argument("--json", action="store_true")
    for name in ("doctor", "components", "validate-manifests"):
        command = sub.add_parser(name)
        command.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
        command.add_argument("--json", action="store_true")
        command.add_argument("--receipt", type=_receipt_path, help="installed-state.json receipt for provenance alignment")
        if name == "doctor":
            command.add_argument("component", nargs="?")
            command.add_argument("--verbose", action="store_true")
            command.add_argument("--report", action="store_true")
            command.add_argument("--preview", action="store_true")

            command.add_argument("--state-dir", type=Path, help="explicit local directory for snapshots and incidents (no persistence by default)")
            command.add_argument("--prefix", type=_absolute_path, help="explicit installation prefix for canonical artifact paths")
            command.add_argument("--libexec", type=_absolute_path, help="explicit libexec directory")
            command.add_argument("--sysconf", type=_absolute_path, help="explicit system configuration directory")
    return parser

from .render import render_acceptance_assessment


def _render_expected_error(args: argparse.Namespace, exc: Exception) -> None:
    message = str(exc)
    if getattr(args, "json", False):
        print(json.dumps({"recommendation": "indeterminate", "error": message}, indent=2, sort_keys=True))
    else:
        print(f"Realmheart Doctor could not assess the candidate: {message}")


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == "--incident":
        arguments[0] = "incident"
    manual = not arguments or arguments[0] != "assess-install"
    try:
        args = _parser(manual=manual).parse_args(arguments)
        if args.command == "doctor" and ((args.report and args.state_dir is None)
                                         or (args.preview and not args.report)):
            raise _InvalidInvocation("invalid_invocation")

    except _InvalidInvocation:
        print(json.dumps({"format_version": 1, "status": "error", "error": "invalid_invocation"})
              if "--json" in arguments else "Invalid invocation; run realmheart-doctor --help")
        return 4
    if args.command == "incident":
        from .incident_reports import load_incident, render_incident, write_report

        try:
            path = None
            if args.report:
                path, report = write_report(args.state_dir, args.incident_id)
            else:
                report = render_incident(load_incident(args.state_dir, args.incident_id))
            payload = {"format_version": 1, **report}
            if path is not None:
                payload["report_path"] = str(path)
            print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                  report["text"] if args.preview or path is None else str(path))
            return 0 if report["export_allowed"] else 5
        except (OSError, ValueError, RecursionError):
            print(json.dumps({"format_version": 1, "error": "incident_report_failed"}) if args.json
                  else "Doctor could not read or safely export the incident")
            return 5
    if args.command != "assess-install":
        return _manual(args)
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


def _manual(args: argparse.Namespace) -> int:
    from .diagnosis import EXIT_CODES, diagnose, render_diagnosis

    try:
        registry = load_manifest(args.manifest_dir)
    except (ManifestError, OSError, ForensicContractError) as exc:
        payload = {"format_version": 1, "status": "error", "error": "manifest_configuration_error"}
        print(json.dumps(payload, sort_keys=True) if args.json else "Doctor manifest configuration error")
        return 5
    if args.command == "doctor":
        if args.component is not None and args.component not in registry.components:
            payload = {"format_version": 1, "status": "error", "error": "unknown component"}
            print(json.dumps(payload) if args.json else "Unknown component; run realmheart-doctor components")
            return 4
        try:
            receipt = load_installed_receipt(args.receipt) if args.receipt else None
        except (ForensicContractError, OSError):
            payload = {"format_version": 1, "status": "error", "error": "receipt_configuration_error"}
            print(json.dumps(payload) if args.json else "Doctor receipt configuration error")
            return 5
        with _installation_environment(args):
            result = diagnose(registry, args.component, receipt=receipt)
        payload = result.to_dict()
        if args.state_dir is not None:
            from .state import record_diagnosis
            from .incidents import record_component_failure

            try:
                state = record_diagnosis(args.state_dir, result)
                events = [record_component_failure(args.state_dir, component.id)
                          for component in result.components]
            except OSError:
                payload["state"] = {"error": "state_persistence_failed"}
                print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                      render_diagnosis(result, verbose=args.verbose) + "\nDoctor state persistence failed")
                return 5
            payload["state"] = {"recovered": list(state.recovered),
                                "incident_ids": [event.incident_id for event in events if event is not None]}
            if args.report:
                from .incident_reports import write_report

                reports = []
                for event in events:
                    if event is None:
                        continue
                    try:
                        path, _ = write_report(args.state_dir, event.incident_id)
                    except (OSError, ValueError, RecursionError):
                        continue
                    reports.append(str(path))
                payload["reports"] = reports
                if args.preview:
                    payload["report_text"] = {Path(path).stem: Path(path).read_text(encoding="utf-8") for path in reports}
        print(json.dumps(payload, indent=2, sort_keys=True) if args.json else render_diagnosis(result, verbose=args.verbose))
        return EXIT_CODES[result.overall]
    components = [{"id": key, "name": registry.components[key].name,
                   "category": registry.components[key].category} for key in registry.component_order]
    payload = {"format_version": 1, "doctor_version": RELEASE_VERSION, "status": "ok", "release_version": registry.release_version,
               "manifest_digest": registry.digest, "components": components}
    print(json.dumps(payload, indent=2, sort_keys=True) if args.json else "\n".join(item["id"] for item in components))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
