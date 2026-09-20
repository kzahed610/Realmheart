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
    overrides = {name.upper(): getattr(args, name, None) for name in ("prefix", "libexec", "sysconf")
                 if getattr(args, name, None) is not None}
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
    from .state import default_state_root

    incident.add_argument("--state-dir", type=Path, default=default_state_root())
    incident.add_argument("--json", action="store_true")
    incident.add_argument("--preview", action="store_true")
    boot = sub.add_parser("boot", help="noninteractive one-shot health check per session")
    boot.add_argument("--state-dir", type=Path, required=True)
    boot.add_argument("--session-key", default=None,
                      help="one-shot marker key (defaults to HYPRLAND_INSTANCE_SIGNATURE, then the machine boot id)")
    boot.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    boot.add_argument("--no-notify", action="store_true")
    boot.add_argument("--json", action="store_true")
    post_update = sub.add_parser("post-update", help="correlate relevant package transactions after a system update")
    post_update.add_argument("--state-dir", type=Path, required=True)
    post_update.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    post_update.add_argument("--since", help="ISO-8601 timestamp overriding the pacman-hook marker")
    post_update.add_argument("--no-notify", action="store_true")
    post_update.add_argument("--json", action="store_true")
    assess = sub.add_parser("assess-install", help="independently assess a candidate Realmheart installation")
    assess.add_argument("--candidate", type=Path, required=True)
    assess.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    assess.add_argument("--json", action="store_true")
    repair = sub.add_parser("repair", help="consent-gated repair of one component (dry run unless --apply)")
    repair.add_argument("component")
    repair.add_argument("--apply", action="store_true", help="execute the plan instead of printing it")
    repair.add_argument("--yes", action="store_true", help="answer yes to CONFIRM actions")
    repair.add_argument("--allow-privileged", action="store_true",
                        help="allow PRIVILEGED_CONFIRM actions after consent")
    repair.add_argument("--build-dir", type=_absolute_path, help="explicit build directory for targeted rebuilds")
    repair.add_argument("--prefix", type=_absolute_path, help="explicit installation prefix for targeted installs")
    repair.add_argument("--manifest-dir", type=Path, default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")))
    repair.add_argument("--receipt", type=_receipt_path, help="installed-state.json receipt (build provenance)")
    repair.add_argument("--state-dir", type=Path, help="explicit local directory for repair records")
    repair.add_argument("--json", action="store_true")
    repair_incident = sub.add_parser(
        "repair-incident",
        help="consent-gated repair for the component named by one saved incident",
    )
    repair_incident.add_argument("incident_id")
    repair_incident.add_argument("--apply", action="store_true",
                                 help="execute the plan instead of printing it")
    repair_incident.add_argument("--yes", action="store_true",
                                 help="answer yes to CONFIRM actions")
    repair_incident.add_argument("--allow-privileged", action="store_true",
                                 help="allow PRIVILEGED_CONFIRM actions after consent")
    repair_incident.add_argument("--build-dir", type=_absolute_path,
                                 help="explicit build directory for targeted rebuilds")
    repair_incident.add_argument("--prefix", type=_absolute_path,
                                 help="explicit installation prefix for targeted installs")
    repair_incident.add_argument(
        "--manifest-dir", type=Path,
        default=Path(os.environ.get("REALMHEART_DOCTOR_MANIFEST_DIR", "components")),
    )
    repair_incident.add_argument("--receipt", type=_receipt_path,
                                 help="installed-state.json receipt (build provenance)")
    repair_incident.add_argument("--state-dir", type=Path, default=default_state_root(),
                                 help="Doctor state directory containing the incident")
    repair_incident.add_argument("--json", action="store_true")
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
            command.add_argument("--integrity", action="store_true",
                                 help="receipt-backed installation integrity view (no new diagnosis)")
            command.add_argument("--explain", action="store_true",
                                 help="add evidence-backed explanations for observed failures")

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
        if args.command == "doctor" and args.integrity and (
            args.component is not None or args.report or args.preview
        ):
            raise _InvalidInvocation("invalid_invocation")

    except _InvalidInvocation:
        print(json.dumps({"format_version": 1, "status": "error", "error": "invalid_invocation"})
              if "--json" in arguments else "Invalid invocation; run realmheart-doctor --help")
        return 4
    if args.command == "boot":
        from .boot import run_boot
        from .notify_backends import deliver, resolve_incident_event

        try:
            registry = load_manifest(args.manifest_dir)
        except (ManifestError, OSError, ForensicContractError):
            payload = {"format_version": 1, "status": "error", "error": "manifest_configuration_error"}
            print(json.dumps(payload, sort_keys=True) if args.json else "Doctor manifest configuration error")
            return 5
        if not args.session_key:
            from .boot import default_session_key

            args.session_key = default_session_key()
        if not args.session_key:
            print(json.dumps({"format_version": 1, "status": "error", "error": "session_key_unavailable"}) if args.json
                  else "Boot mode needs a session key; pass --session-key")
            return 4
        notified = 0

        def _notifier(
            title: str, body: str, severity: str = "warning", **metadata,
        ) -> None:
            # A suppressed or failed delivery must stay retry-eligible: raising
            # keeps dispatch from recording last_notified_state.
            nonlocal notified
            if not deliver(title, body, severity=severity, **metadata):
                raise RuntimeError("notification delivery failed")
            notified += 1

        outcome = run_boot(
            registry, args.state_dir, session_key=args.session_key,
            notifier=None if args.no_notify else _notifier,
            resolver=None if args.no_notify else resolve_incident_event,
        )
        payload = {"format_version": 1, "mode": outcome.mode, "notifications": notified}
        print(json.dumps(payload, sort_keys=True) if args.json else f"boot: {outcome.mode}")
        return 0
    if args.command == "post-update":
        from datetime import datetime, timezone

        from .post_update import run_post_update

        try:
            registry = load_manifest(args.manifest_dir)
        except (ManifestError, OSError, ForensicContractError):
            payload = {"format_version": 1, "status": "error", "error": "manifest_configuration_error"}
            print(json.dumps(payload, sort_keys=True) if args.json else "Doctor manifest configuration error")
            return 5
        since = None
        if args.since:
            try:
                since = datetime.fromisoformat(args.since)
            except ValueError:
                payload = {"format_version": 1, "status": "error", "error": "invalid_invocation"}
                print(json.dumps(payload, sort_keys=True) if args.json else
                      "Invalid --since timestamp; expected ISO-8601")
                return 4
            if since.tzinfo is None:
                since = since.replace(tzinfo=timezone.utc)
        notifier = None
        resolver = None
        if not args.no_notify:
            from .notify_backends import deliver, resolve_incident_event
            resolver = resolve_incident_event

            def notifier(
                title: str, body: str, severity: str = "warning", **metadata,
            ) -> None:
                if not deliver(title, body, severity=severity, **metadata):
                    raise RuntimeError("notification delivery failed")

        outcome = run_post_update(
            registry, args.state_dir, notifier=notifier, resolver=resolver, since=since,
        )
        payload = outcome.to_dict()
        if args.json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            print(f"post-update: {outcome.mode}")
            if outcome.report is not None:
                for component in outcome.report.affected_components:
                    print(f"  affected: {component}")
                for transaction in outcome.report.transactions:
                    print(f"  {transaction.get('package')}: {transaction.get('previous')} -> {transaction.get('current')}")
        return 0
    if args.command == "incident":
        from .incident_reports import load_incident, render_incident, write_report

        try:
            path = None
            if args.report:
                from .locking import acquire_state_lock

                with acquire_state_lock(args.state_dir, timeout=1.0):
                    path, report = write_report(args.state_dir, args.incident_id)
            else:
                report = render_incident(load_incident(args.state_dir, args.incident_id))
            payload = {"format_version": 1, **report}
            if path is not None:
                payload["report_path"] = str(path)
            print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                  report["text"] if args.preview or path is None else str(path))
            return 0 if report["export_allowed"] else 5
        except TimeoutError:
            print(json.dumps({"format_version": 1, "error": "state_busy"}) if args.json
                  else "Doctor state is busy; another operation is active")
            return 5
        except (OSError, ValueError, RecursionError):
            print(json.dumps({"format_version": 1, "error": "incident_report_failed"}) if args.json
                  else "Doctor could not read or safely export the incident")
            return 5
    if args.command == "repair-incident":
        from .incident_reports import load_incident

        try:
            incident_payload = load_incident(args.state_dir, args.incident_id)
        except (OSError, ValueError, RecursionError):
            payload = {"format_version": 1, "status": "error", "error": "incident_not_found"}
            print(json.dumps(payload, sort_keys=True) if args.json else
                  "Doctor could not load that incident")
            return 4
        if incident_payload.get("resolution_state") != "unresolved":
            payload = {"format_version": 1, "status": "error", "error": "incident_resolved"}
            print(json.dumps(payload, sort_keys=True) if args.json else
                  "That Doctor incident is already resolved")
            return 3
        component_id = incident_payload.get("component_id")
        if not isinstance(component_id, str) or not component_id:
            payload = {"format_version": 1, "status": "error", "error": "incident_invalid"}
            print(json.dumps(payload, sort_keys=True) if args.json else
                  "Doctor incident does not identify a component")
            return 5
        args.command = "repair"
        args.component = component_id
        if args.receipt is None:
            candidate = args.state_dir.parent / "installed-state.json"
            if candidate.is_file():
                args.receipt = candidate
        return _manual(args)
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


def _consent_callback(args):
    """Interactive consent for non-SAFE actions; never granted implicitly."""

    def consent(action) -> bool:
        privileged = action.risk == "PRIVILEGED_CONFIRM"
        if privileged and not args.allow_privileged:
            print("Doctor will not run privileged repairs without --allow-privileged", file=sys.stderr)
            return False
        if not sys.stdin.isatty():
            return bool(args.yes)
        print(f"Doctor wants to perform: {action.description}", file=sys.stderr)
        try:
            answer = input().strip().lower()
        except EOFError:
            return False
        return answer in {"y", "yes"}

    return consent


def _resolve_default_state_events(state_dir: Path | None, incident_ids) -> None:
    """Best-effort mirror of verified recovery into the live Event Surface.

    Event Surface notifications are emitted only for the canonical Doctor state
    tree.  Keeping custom/test state directories isolated also prevents a
    diagnostic fixture from touching the user's real eventd instance.
    """

    if state_dir is None:
        return
    from .state import default_state_root

    try:
        if Path(state_dir).resolve(strict=False) != default_state_root().resolve(strict=False):
            return
    except OSError:
        return
    try:
        from .notify_backends import resolve_incident_event
    except Exception:
        return
    for incident_id in incident_ids:
        try:
            resolve_incident_event(str(incident_id))
        except Exception:
            pass


def _repair_exit_code(report) -> int:
    if report.verified:
        return 0
    succeeded = any(item.status == "succeeded" for item in report.executions)
    refused = any(item.status == "skipped_no_consent" for item in report.executions)
    if refused and not succeeded:
        return 4
    if report.component_status == "failed":
        return 2
    return 3


def _repair(args: argparse.Namespace, registry) -> int:
    """Run repair under the Doctor state lock when it can mutate state."""

    if not args.apply or args.state_dir is None:
        return _repair_transaction(args, registry)
    from .locking import acquire_state_lock

    try:
        with acquire_state_lock(args.state_dir, timeout=1.0):
            return _repair_transaction(args, registry)
    except TimeoutError:
        payload = {"format_version": 1, "status": "error", "error": "state_busy"}
        print(json.dumps(payload, sort_keys=True) if args.json else
              "Doctor state is busy; another operation is active")
        return 5
    except OSError:
        payload = {"format_version": 1, "status": "error", "error": "state_persistence_failed"}
        print(json.dumps(payload, sort_keys=True) if args.json else
              "Doctor state persistence failed")
        return 5


def _repair_transaction(args: argparse.Namespace, registry) -> int:
    from .classification import classify_failure
    from .diagnosis import diagnose
    from .incidents import record_component_recovery, record_repair_attempt, recorded_attempt_fingerprints
    from .repair import RepairContext, assess_repair_evidence, plan_repairs
    from .repair_runners import default_repair_runners, render_repair_plan, render_repair_report, run_repair_plan

    if args.component not in registry.components:
        payload = {"format_version": 1, "status": "error", "error": "unknown component"}
        print(json.dumps(payload) if args.json else "Unknown component; run realmheart-doctor components")
        return 4
    try:
        receipt = load_installed_receipt(args.receipt) if args.receipt else None
    except (ForensicContractError, OSError):
        payload = {"format_version": 1, "status": "error", "error": "receipt_configuration_error"}
        print(json.dumps(payload) if args.json else "Doctor receipt configuration error")
        return 5

    def _diagnose():
        with _installation_environment(args):
            return diagnose(registry, args.component, receipt=receipt)

    diagnosis = _diagnose()
    component = next(item for item in diagnosis.components if item.id == args.component)
    evidence = assess_repair_evidence(registry, args.component)
    classification = classify_failure(
        component.checks,
        missing_capabilities=evidence.missing_capability_ids,
        failed_capabilities=evidence.failed_capability_ids,
    )
    spec = registry.components[args.component]
    plan = plan_repairs(args.component, classification, context=RepairContext(
        component_id=args.component,
        strategy_ids=spec.repair_strategy_ids,
        packages=evidence.packages,
        gated_packages=evidence.gated_packages,
        build_targets=evidence.build_targets,
        service_units=evidence.service_units,
        installer_bound=spec.requires_installer_binding,
        notes=evidence.notes,
    ))
    if plan is None:
        payload = {
            "format_version": 1, "mode": "no_plan", "component": args.component,
            "status": component.status.value, "failure_class": classification.failure_class,
            "notes": list(evidence.notes),
        }
        if args.json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        elif component.status.value == "healthy":
            print(f"{args.component} is healthy; no repair was planned")
        else:
            print(f"No executable repair is available for {args.component} "
                  f"({classification.failure_class}).")
            for note in evidence.notes:
                print(f"  note: {note}")
        return 0 if component.status.value == "healthy" else 3
    if not args.apply:
        payload = {"format_version": 1, "mode": "dry_run", "plan": plan.to_dict()}
        print(json.dumps(payload, indent=2, sort_keys=True) if args.json else render_repair_plan(plan))
        return 0
    repair_incident = None
    if args.state_dir is not None:
        from .incidents import record_component_failure
        from .journal import journal
        from .log_evidence import log_collector_for
        from .state import record_diagnosis

        # Bind the repair to the exact failure snapshot that produced its plan.
        # This also makes replay protection incident-local instead of component-global.
        record_diagnosis(args.state_dir, diagnosis)
        repair_incident = record_component_failure(
            args.state_dir, args.component, log_collector=log_collector_for(registry),
        )
        journal(args.state_dir, "repair_planned", component=args.component,
                failure_class=classification.failure_class, actions=len(plan.actions),
                incident=(repair_incident.incident_id if repair_incident else None))

    provenance = receipt.build_provenance if receipt is not None else None
    build_dir = args.build_dir or (provenance.cmake_binary_dir if provenance is not None else None)
    prefix = args.prefix or (provenance.cmake_install_prefix if provenance is not None else None)
    runners = default_repair_runners(
        component_id=args.component, build_dir=build_dir, prefix=prefix,
        installer_bound=spec.requires_installer_binding,
    )
    attempted = (
        recorded_attempt_fingerprints(
            args.state_dir, args.component,
            incident_id=repair_incident.incident_id if repair_incident else None,
        )
        if args.state_dir and repair_incident is not None else ()
    )

    def verifier():
        post = _diagnose()
        entry = next(item for item in post.components if item.id == args.component)
        if args.state_dir is not None:
            from .state import record_diagnosis

            try:
                record_diagnosis(args.state_dir, post, resolve_incidents=False)
            except OSError:
                pass
        detail = ", ".join(entry.uncertainties) or f"{len(entry.checks)} checks re-run"
        return entry.status.value, detail

    report = run_repair_plan(plan, consent=_consent_callback(args), runners=runners,
                             verifier=verifier, attempted=attempted)
    if args.state_dir is not None:
        try:
            record_repair_attempt(
                args.state_dir, args.component,
                tuple(item.to_dict() for item in report.executions),
                outcome=report.component_status or "unknown",
                incident_id=repair_incident.incident_id if repair_incident else None,
            )
            if report.verified:
                from .incidents import unresolved_incident_ids

                resolved_event_ids = unresolved_incident_ids(args.state_dir, args.component)
                record_component_recovery(args.state_dir, args.component)
                _resolve_default_state_events(args.state_dir, resolved_event_ids)
        except OSError:
            payload = {"format_version": 1, "status": "error", "error": "state_persistence_failed",
                       "plan": plan.to_dict(), "report": report.to_dict()}
            print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                  render_repair_report(report) + "\nDoctor state persistence failed")
            return 5
    payload = {"format_version": 1, "mode": "applied", "plan": plan.to_dict(), "report": report.to_dict()}
    if args.state_dir is not None:
        from .journal import journal

        journal(args.state_dir, "repair_executed", component=args.component, verified=report.verified,
                statuses=",".join(item.status for item in report.executions))
        from .retention import apply_retention

        apply_retention(args.state_dir)
    print(json.dumps(payload, indent=2, sort_keys=True) if args.json else render_repair_report(report))
    return _repair_exit_code(report)


def _manual(args: argparse.Namespace) -> int:
    from .diagnosis import EXIT_CODES, diagnose, render_diagnosis

    try:
        registry = load_manifest(args.manifest_dir)
    except (ManifestError, OSError, ForensicContractError) as exc:
        payload = {"format_version": 1, "status": "error", "error": "manifest_configuration_error"}
        print(json.dumps(payload, sort_keys=True) if args.json else "Doctor manifest configuration error")
        return 5
    if args.command == "repair":
        return _repair(args, registry)
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
        if args.integrity:
            if receipt is None:
                payload = {"format_version": 1, "status": "error", "error": "integrity_receipt_required"}
                print(json.dumps(payload) if args.json
                      else "Integrity mode needs an accepted receipt; pass --receipt")
                return 3
            from .integrity import assess_integrity, render_integrity

            with _installation_environment(args):
                report = assess_integrity(registry, receipt)
            print(json.dumps(report.to_dict(), indent=2, sort_keys=True) if args.json else render_integrity(report))
            return {"clean": 0, "attention": 1, "drift": 2}.get(report.status, 3)
        def _diagnose_manual():
            with _installation_environment(args):
                return diagnose(registry, args.component, receipt=receipt)

        def _payload_for(result):
            payload = result.to_dict()
            if args.explain:
                from .classification import classify_component
                from .explanations import explanation_for

                explanations = []
                for component in result.components:
                    if component.status.value == "healthy":
                        continue
                    classification = classify_component(component)
                    explanations.append({
                        "component": component.id,
                        "status": component.status.value,
                        "failure_class": classification.failure_class,
                        "evidence": list(classification.evidence_ids),
                        "explanation": explanation_for(classification.failure_class,
                                                       classification.evidence_ids)
                        or "No explanation template covers this failure yet; the raw evidence stands.",
                    })
                payload["explanations"] = explanations
            return payload

        if args.state_dir is None:
            result = _diagnose_manual()
            payload = _payload_for(result)
        else:
            from .incidents import record_component_failure
            from .locking import acquire_state_lock
            from .state import record_diagnosis

            result = None
            payload = None
            resolved_event_ids: tuple[str, ...] = ()
            try:
                # Keep the diagnosis and every state mutation in one transaction.
                # Acquiring only around the writes would allow a slow stale probe
                # to overwrite a newer repair/boot result after that writer exits.
                with acquire_state_lock(args.state_dir, timeout=1.0):
                    result = _diagnose_manual()
                    payload = _payload_for(result)
                    state = record_diagnosis(args.state_dir, result)
                    resolved_event_ids = state.resolved_incidents
                    from .log_evidence import log_collector_for

                    collector = log_collector_for(registry)
                    events = [
                        record_component_failure(
                            args.state_dir, component.id, log_collector=collector
                        )
                        for component in result.components
                    ]
                    payload["state"] = {
                        "recovered": list(state.recovered),
                        "incident_ids": [
                            event.incident_id for event in events if event is not None
                        ],
                    }
                    if args.report:
                        from .incident_reports import write_report

                        reports = []
                        for event in events:
                            if event is None:
                                continue
                            try:
                                report_path, _ = write_report(args.state_dir, event.incident_id)
                            except (OSError, ValueError, RecursionError):
                                continue
                            reports.append(str(report_path))
                        payload["reports"] = reports
                        if args.preview:
                            payload["report_text"] = {
                                Path(report_path).stem: Path(report_path).read_text(encoding="utf-8")
                                for report_path in reports
                            }
                    from .retention import apply_retention

                    payload["state"]["retention"] = apply_retention(args.state_dir)
            except TimeoutError:
                payload = {"format_version": 1, "state": {"error": "state_busy"}}
                print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                      "Doctor state is busy; another operation is active")
                return 5
            except OSError:
                # Persistence may be unavailable (for example, a path component
                # is a regular file).  Keep diagnosis useful and read-only even
                # when the state sink itself is broken.  A lock contention is
                # different: that path returns above without probing so stale
                # evidence cannot race a live writer.
                if result is None:
                    result = _diagnose_manual()
                    payload = _payload_for(result)
                assert payload is not None
                payload["state"] = {"error": "state_persistence_failed"}
                print(json.dumps(payload, indent=2, sort_keys=True) if args.json else
                      render_diagnosis(result, verbose=args.verbose) +
                      "\nDoctor state persistence failed")
                return 5
            _resolve_default_state_events(args.state_dir, resolved_event_ids)
        if args.json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            text = render_diagnosis(result, verbose=args.verbose)
            if args.explain and payload.get("explanations"):
                from .explanations import render_explanations

                text += "\n\n" + render_explanations(tuple(payload["explanations"]))
            print(text)
        return EXIT_CODES[result.overall]
    if args.command == "validate-manifests":
        from realmheart_maintenance.repository import validate_repository

        root = Path(args.manifest_dir).resolve().parent
        repository_checked = (root / "CMakeLists.txt").is_file()
        validation = validate_repository(root, registry) if repository_checked else None
        ok = validation is None or validation.ok
        payload = {
            "format_version": 1,
            "doctor_version": RELEASE_VERSION,
            "status": "ok" if ok else "error",
            "release_version": registry.release_version,
            "manifest_digest": registry.digest,
            "components": len(registry.components),
            "dependencies": len(registry.dependencies),
            "capabilities": len(registry.capabilities),
            "artifacts": len(registry.artifacts),
            "health_checks": len(registry.health_checks),
            "build_units": len(registry.build_units),
            "repository": None if validation is None else {
                "checked": True,
                "ok": validation.ok,
                "errors": list(validation.errors),
                "warnings": list(validation.warnings),
            },
        }
        if args.json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            print(f"Canonical manifest: {'PASS' if ok else 'FAIL'}")
            print(f"  Release: {registry.release_version}")
            print(f"  Digest: {registry.digest}")
            print(f"  Graph: {len(registry.components)} components, {len(registry.dependencies)} dependencies, "
                  f"{len(registry.capabilities)} capabilities, {len(registry.artifacts)} artifacts, "
                  f"{len(registry.build_units)} build units, {len(registry.health_checks)} health checks")
            if validation is not None:
                if not repository_checked:
                    print("  repository: not a Realmheart checkout; manifest validated on its own")
                for error in validation.errors:
                    print(f"  error: {error}")
        return 0 if ok else 5
    components = [{"id": key, "name": registry.components[key].name,
                   "category": registry.components[key].category} for key in registry.component_order]
    payload = {"format_version": 1, "doctor_version": RELEASE_VERSION, "status": "ok", "release_version": registry.release_version,
               "manifest_digest": registry.digest, "components": components}
    print(json.dumps(payload, indent=2, sort_keys=True) if args.json else "\n".join(item["id"] for item in components))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
