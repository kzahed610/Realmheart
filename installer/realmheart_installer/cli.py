"""Realmheart Installer command-line entry point."""

from __future__ import annotations

import argparse
import json
import signal
import sys
import tempfile
from contextlib import nullcontext
from pathlib import Path

from realmheart_doctor import render_acceptance_assessment
from realmheart_maintenance.github_issues import offer_github_issue
from realmheart_maintenance.manifest import load_manifest

from .constants import INSTALLER_VERSION
from .components.handlers import resolve_component_handler_specs
from .components.render import render_component_footprints
from .context import InstallContext, XdgPaths, ensure_not_root, generate_transaction_id
from .diagnostics import (
    DiagnosticReportBuilder, DiagnosticReportStore, build_failure_report,
    render_json_report, render_markdown_report,
)
from .environment.command import CommandRunner
from .environment.preflight import PreflightScanner
from .environment.render import render_preflight
from .errors import InstallerError, TerminationSignalInterrupt, interruption_reason
from .finalization import FinalAction, render_final_decision, render_finalization_result
from .live import LiveInstallExecutor
from .models import TransactionState, to_jsonable
from .native_build import NativeBuildExecutor, render_build_stage_report
from .package_manager.pacman import PacmanAdapter, build_pacman_dependency_plan, package_required_by
from .package_manager.render import render_install_result, render_package_plan
from .planning import InstallationPlanner
from .planning.render import render_installation_plan
from .verification import VerificationEngine, render_verification_report
from .transaction.lock import InstallerLock
from .transaction.recovery import (
    RecoveryStatus,
    acknowledge_manual_recovery,
    discover_recovery_candidates,
    persist_recovery_report,
    recover_transaction_from_journal,
)
from .uninstall import (
    UninstallConfigAction, UninstallExecutor, UninstallPlanner,
    render_uninstall_compare, render_uninstall_plan, render_uninstall_result,
)


_SOURCE_ROOT = Path(__file__).resolve().parents[2]


def _raise_termination_interrupt(signum: int, _frame) -> None:
    raise TerminationSignalInterrupt(signum)


def _install_termination_signal_handler():
    """Translate SIGTERM into the normal controlled interruption path.

    ``signal.signal`` is only legal in Python's main thread.  Realmheart's CLI is
    expected to run there; if an embedding test/application invokes ``main`` from
    another thread we simply leave the host signal policy untouched.
    """

    try:
        previous = signal.getsignal(signal.SIGTERM)
        signal.signal(signal.SIGTERM, _raise_termination_interrupt)
    except (ValueError, OSError, AttributeError):
        return None
    return previous


def _restore_termination_signal_handler(previous) -> None:
    if previous is None:
        return
    try:
        signal.signal(signal.SIGTERM, previous)
    except (ValueError, OSError, AttributeError):
        pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="realmheart-installer",
        description="Realmheart transactional installer",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {INSTALLER_VERSION}")
    parser.add_argument("--dry-run", action="store_true", help="build the authoritative installation plan; never mutate Realmheart installation state")
    parser.add_argument("--verbose", action="store_true", help="show developer-oriented plan detail")
    parser.add_argument("--json", action="store_true", help="emit the selected command result as JSON")
    parser.add_argument("--report-path", type=Path, help="override diagnostic report output directory")
    parser.add_argument("--report-id", help="diagnostic incident id for report-inspect/report-remove")
    parser.add_argument("--transaction-id", help="transaction id for recovery-inspect/recovery-rollback/recovery-acknowledge")
    parser.add_argument("--install-dependencies", action="store_true", help="with dependencies: allow pacman to install the proposed missing packages")
    parser.add_argument("--yes", action="store_true", help="confirm proposed package/live installation without the initial confirmation prompt")
    parser.add_argument("--decision", choices=[item.value for item in FinalAction], help="explicit final action for degraded/failed installs")
    parser.add_argument("--uninstall-config", choices=[item.value for item in UninstallConfigAction], help="uninstall config policy: keep current config or restore the permanent pre-Realmheart baseline")
    parser.add_argument("--compare-config", action="store_true", help="with uninstall: show current-vs-baseline path changes and exit without mutation")
    parser.add_argument("--cleanup-dependencies", action="store_true", help="with uninstall: attempt exact removal only for packages proven installed by the Realmheart transaction")
    parser.add_argument("--purge-event-history", action="store_true", help="with uninstall: explicitly remove Event Surface events.db instead of preserving it")
    parser.add_argument("command", nargs="?", choices=["preflight", "dependencies", "build-stage", "component-plan", "verify-current", "diagnose-current", "report-list", "report-inspect", "report-remove", "recovery-list", "recovery-inspect", "recovery-rollback", "recovery-acknowledge", "install", "uninstall"], help="installer action")
    return parser


def _validate_cli_contract(args: argparse.Namespace) -> None:
    """Reject flag combinations that would otherwise be ignored or unsafe.

    The CLI deliberately uses one compact parser rather than a large subparser
    tree, so command-specific flags must fail closed instead of silently doing
    nothing on an unrelated command.  In particular, ``--dry-run`` must never
    be accepted by a command that can persist/remove recovery or report state.
    """

    command = args.command
    if args.dry_run and command in {
        "report-remove", "recovery-list", "recovery-inspect",
        "recovery-rollback", "recovery-acknowledge",
    }:
        raise InstallerError(
            f"--dry-run is not valid with {command}; this command has its own explicit recovery/report semantics",
            code="RH_CLI_FLAG_CONFLICT", stage="cli",
        )

    command_flags = (
        (args.report_id is not None, {"report-inspect", "report-remove"}, "--report-id"),
        (args.transaction_id is not None, {"recovery-inspect", "recovery-rollback", "recovery-acknowledge"}, "--transaction-id"),
        (args.install_dependencies, {"dependencies", "install"}, "--install-dependencies"),
        (args.decision is not None, {"install"}, "--decision"),
        (args.uninstall_config is not None, {"uninstall"}, "--uninstall-config"),
        (args.compare_config, {"uninstall"}, "--compare-config"),
        (args.cleanup_dependencies, {"uninstall"}, "--cleanup-dependencies"),
        (args.purge_event_history, {"uninstall"}, "--purge-event-history"),
        (args.report_path is not None, {"diagnose-current"}, "--report-path"),
        (args.yes, {"dependencies", "install", "uninstall", "recovery-acknowledge"}, "--yes"),
    )
    for enabled, allowed, flag in command_flags:
        if enabled and command not in allowed:
            allowed_text = ", ".join(sorted(allowed))
            raise InstallerError(
                f"{flag} is only valid with: {allowed_text}",
                code="RH_CLI_FLAG_CONFLICT", stage="cli",
            )

    if command == "uninstall" and args.compare_config and args.dry_run:
        raise InstallerError(
            "--compare-config and --dry-run are separate uninstall inspection modes; choose one",
            code="RH_CLI_FLAG_CONFLICT", stage="cli",
        )
    if command == "uninstall" and (args.dry_run or args.compare_config):
        ignored = []
        if args.uninstall_config is not None:
            ignored.append("--uninstall-config")
        if args.cleanup_dependencies:
            ignored.append("--cleanup-dependencies")
        if args.purge_event_history:
            ignored.append("--purge-event-history")
        if ignored:
            raise InstallerError(
                f"{', '.join(ignored)} require a live uninstall and cannot be combined with inspection-only mode",
                code="RH_CLI_FLAG_CONFLICT", stage="cli",
            )
    if args.dry_run and args.decision is not None:
        raise InstallerError(
            "--decision applies only to live finalization and cannot be combined with --dry-run",
            code="RH_CLI_FLAG_CONFLICT", stage="cli",
        )


def _should_offer_failure_report(exc: BaseException) -> bool:
    if not isinstance(exc, InstallerError):
        return True
    # Operator/invocation/recovery state is useful to record locally but should
    # not nudge users toward filing product bugs.
    if exc.stage in {"cli", "startup", "recovery"}:
        return False
    if exc.code.endswith("_CONFIRMATION_REQUIRED") or exc.code.endswith("_ID_REQUIRED"):
        return False
    return True


def _persist_terminal_failure_report(
    *,
    args: argparse.Namespace,
    exc: BaseException,
    paths: XdgPaths | None,
    transaction_id: str | None,
    snapshot,
    plan,
) -> None:
    """Best-effort incident persistence that never masks the primary error."""
    if paths is None or transaction_id is None:
        return
    try:
        payload, markdown, github, title = build_failure_report(
            exc,
            transaction_id=transaction_id,
            operation=args.command or "plan",
            snapshot=snapshot,
            plan=plan,
        )
        bundle = DiagnosticReportStore(paths).save_rendered(
            str(payload["incident_id"]),
            payload=payload,
            markdown=markdown,
            github=github,
        )
        print(f"Diagnostic incident: {bundle.incident_id}", file=sys.stderr)
        print(f"Machine-authored report: {bundle.github_path}", file=sys.stderr)
        if not args.json and _should_offer_failure_report(exc):
            offer_github_issue(title, github, report_path=bundle.github_path)
    except Exception as report_exc:
        # Diagnostics are secondary.  Never replace the real installer failure
        # with a report-generation/browser error.
        print(
            f"WARNING [RH_DIAGNOSTIC_REPORT_FAILED]: {type(report_exc).__name__}",
            file=sys.stderr,
        )


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    context: InstallContext | None = None
    paths: XdgPaths | None = None
    transaction_id: str | None = None
    snapshot = None
    plan = None
    previous_sigterm_handler = _install_termination_signal_handler()

    try:
        _validate_cli_contract(args)
        ensure_not_root()
        paths = XdgPaths.resolve()
        paths.prepare_lock_parent()
        transaction_id = generate_transaction_id()
        runner = CommandRunner()

        # Runtime locking is permitted in dry-run. The advisory lock metadata
        # file may persist; unlinking a flock path after release creates an inode
        # race that can admit concurrent installers. Dry-run still creates no
        # transaction directory, backup, package mutation, or Realmheart state.
        with InstallerLock(paths.lock_path, transaction_id=transaction_id):
            report_store = DiagnosticReportStore(paths)
            if args.command in {"report-list", "report-inspect", "report-remove"}:
                if args.command == "report-list":
                    bundles = report_store.list()
                    if args.json:
                        print(json.dumps({"reports": [item.incident_id for item in bundles]}, indent=2, sort_keys=True))
                    else:
                        print("Realmheart diagnostic reports")
                        if bundles:
                            for item in bundles:
                                print(f"  {item.incident_id}")
                        else:
                            print("  none")
                    return 0
                if not args.report_id:
                    raise InstallerError("--report-id is required for this command.", code="RH_DIAGNOSTIC_ID_REQUIRED", stage="diagnostics")
                if args.command == "report-inspect":
                    bundle, payload, markdown = report_store.inspect(args.report_id)
                    if args.json:
                        print(json.dumps(payload, indent=2, sort_keys=True))
                    else:
                        print(markdown.rstrip())
                        print(f"\nStored bundle: {bundle.directory}")
                    return 0
                report_store.remove(args.report_id)
                if args.json:
                    print(json.dumps({"removed": args.report_id}, indent=2, sort_keys=True))
                else:
                    print(f"Removed diagnostic report {args.report_id}")
                return 0

            if args.command in {"recovery-list", "recovery-inspect", "recovery-rollback", "recovery-acknowledge"}:
                candidates = discover_recovery_candidates(paths, persist_reports=True)
                if args.command == "recovery-list":
                    if args.json:
                        print(json.dumps({"recovery_candidates": to_jsonable(candidates)}, indent=2, sort_keys=True))
                    else:
                        print(_render_recovery_candidates(candidates))
                    return 23 if candidates else 0

                if not args.transaction_id:
                    raise InstallerError(
                        "--transaction-id is required for this recovery command.",
                        code="RH_RECOVERY_TRANSACTION_ID_REQUIRED", stage="recovery",
                    )
                candidate = next((item for item in candidates if item.transaction_id == args.transaction_id), None)
                if candidate is None:
                    raise InstallerError(
                        f"No unfinished recovery candidate named {args.transaction_id} was found.",
                        code="RH_RECOVERY_TRANSACTION_NOT_FOUND", stage="recovery",
                    )
                if candidate.load_error:
                    if args.json:
                        print(json.dumps(to_jsonable(candidate), indent=2, sort_keys=True))
                    else:
                        print(_render_recovery_candidates((candidate,)))
                    return 24

                recovery_context = InstallContext.load_existing(paths=paths, transaction_id=args.transaction_id)
                if args.command == "recovery-acknowledge":
                    if not args.yes:
                        raise InstallerError(
                            "recovery-acknowledge requires --yes because it records manual resolution without performing repair",
                            code="RH_RECOVERY_ACKNOWLEDGE_CONFIRMATION_REQUIRED", stage="recovery",
                        )
                    recovery = acknowledge_manual_recovery(recovery_context)
                    if args.json:
                        print(json.dumps(to_jsonable(recovery), indent=2, sort_keys=True))
                    else:
                        print(_render_recovery_report(recovery))
                    return 0
                if args.command == "recovery-inspect":
                    recovery = persist_recovery_report(recovery_context, trigger="explicit_recovery_inspect")
                    if args.json:
                        print(json.dumps(to_jsonable(recovery), indent=2, sort_keys=True))
                    else:
                        print(_render_recovery_report(recovery))
                    return 23 if recovery.status is RecoveryStatus.RECOVERY_AVAILABLE else 24

                recovery = recover_transaction_from_journal(recovery_context)
                if args.json:
                    print(json.dumps(to_jsonable(recovery), indent=2, sort_keys=True))
                else:
                    print(_render_recovery_report(recovery))
                return 0 if recovery.status is RecoveryStatus.CLEAN else 24

            read_only = (
                args.dry_run
                or args.command is None
                or args.command in {"preflight", "component-plan", "verify-current", "diagnose-current"}
                or (args.command == "dependencies" and not args.install_dependencies)
                or (args.command == "uninstall" and args.compare_config)
            )
            if not read_only:
                recovery_candidates = discover_recovery_candidates(paths, persist_reports=True)
                blocking_recovery = tuple(item for item in recovery_candidates if item.blocks_new_transaction)
                if blocking_recovery:
                    newest = blocking_recovery[-1]
                    raise InstallerError(
                        "An unfinished Realmheart transaction requires recovery before a new transaction can start. "
                        f"Transaction: {newest.transaction_id}. Run 'recovery-inspect --transaction-id {newest.transaction_id}' first.",
                        code="RH_UNFINISHED_TRANSACTION",
                        stage="recovery",
                        details={"transactions": [item.transaction_id for item in blocking_recovery]},
                    )
            context = None if read_only else InstallContext.create(
                paths=paths,
                source_root=_SOURCE_ROOT,
                dry_run=False,
                transaction_id=transaction_id,
            )

            if args.command == "uninstall":
                uninstall_plan = UninstallPlanner(
                    paths=paths, source_root=_SOURCE_ROOT, transaction_id=transaction_id,
                ).build()
                if args.dry_run:
                    if args.json:
                        print(json.dumps(to_jsonable(uninstall_plan), indent=2, sort_keys=True))
                    else:
                        print(render_uninstall_plan(uninstall_plan, dry_run=True, verbose=args.verbose))
                    return 0 if uninstall_plan.ready else 30
                if args.compare_config:
                    if args.json:
                        print(json.dumps({"plan": to_jsonable(uninstall_plan), "comparisons": to_jsonable(uninstall_plan.comparisons)}, indent=2, sort_keys=True))
                    else:
                        print(render_uninstall_compare(uninstall_plan))
                    return 0 if uninstall_plan.ready else 30
                if not uninstall_plan.ready:
                    if context is not None:
                        context.transaction.transition(TransactionState.FAILED)
                        context.transaction.metadata["scope"] = "phase17_uninstall"
                        context.transaction.metadata["uninstall_blockers"] = list(uninstall_plan.blockers)
                        context.persist_summary()
                    if args.json:
                        print(json.dumps(to_jsonable(uninstall_plan), indent=2, sort_keys=True))
                    else:
                        print(render_uninstall_plan(uninstall_plan, dry_run=False, verbose=args.verbose))
                    return 30

                if args.uninstall_config:
                    config_action = UninstallConfigAction(args.uninstall_config)
                elif args.json:
                    raise InstallerError(
                        "--json uninstall requires --uninstall-config so machine-readable execution never hides a destructive choice",
                        code="RH_UNINSTALL_DECISION_REQUIRED", stage="uninstall",
                    )
                else:
                    config_action = _select_uninstall_config(uninstall_plan)

                if config_action is UninstallConfigAction.RESTORE_BASELINE and not uninstall_plan.baseline_valid:
                    raise InstallerError(
                        "--uninstall-config restore-baseline requires a valid permanent baseline",
                        code="RH_UNINSTALL_BASELINE_UNAVAILABLE", stage="uninstall",
                    )

                if not args.yes:
                    if args.json:
                        raise InstallerError(
                            "live JSON uninstall requires --yes after reviewing the uninstall plan",
                            code="RH_UNINSTALL_CONFIRMATION_REQUIRED", stage="uninstall",
                        )
                    print(render_uninstall_plan(uninstall_plan, dry_run=False, verbose=args.verbose))
                    print("")
                    print(f"Selected configuration action: {config_action.value}")
                    if args.purge_event_history:
                        print("Event Surface history purge: ENABLED (events.db will not be preserved)")
                    if args.cleanup_dependencies and uninstall_plan.package_cleanup_candidates:
                        print("Dependency cleanup: ENABLED for provenance-proven historical Realmheart packages")
                    if not _confirm("Proceed with transactional Realmheart uninstall? [y/N] "):
                        assert context is not None
                        context.transaction.transition(TransactionState.FAILED)
                        context.transaction.metadata["scope"] = "phase17_uninstall"
                        context.transaction.metadata["cancelled_before_uninstall_mutation"] = True
                        context.persist_summary()
                        print("Uninstall cancelled before Realmheart mutation.")
                        return 11

                assert context is not None
                result = UninstallExecutor(
                    plan=uninstall_plan, context=context, paths=paths, runner=runner,
                    config_action=config_action, cleanup_dependencies=args.cleanup_dependencies,
                    purge_event_history=args.purge_event_history,
                ).run()
                if args.json:
                    print(json.dumps(to_jsonable(result), indent=2, sort_keys=True))
                else:
                    print(render_uninstall_result(result))
                return result.exit_code

            if context is not None:
                context.transaction.transition(TransactionState.PREFLIGHT)
                context.persist_summary()

            # Capability compile/link probes need scratch files.  A dry-run uses
            # OS temporary storage so it does not create Realmheart cache state.
            temp_ctx = tempfile.TemporaryDirectory(prefix="realmheart-preflight-") if read_only else nullcontext(None)
            with temp_ctx as temp_dir:
                probe_root = Path(temp_dir) if temp_dir else None
                scanner = PreflightScanner(
                    paths=paths,
                    source_root=_SOURCE_ROOT,
                    runner=runner,
                    probe_temp_root=probe_root,
                )
                snapshot = scanner.scan()

            if context is not None:
                context.transaction.install_mode = snapshot.installation.mode
                context.transaction.installation_origin = snapshot.installation.origin.value
                context.transaction.current_version = snapshot.installation.installed_version_text
                context.transaction.target_version = snapshot.installation.source.version_text
                context.transaction.metadata["preflight"] = to_jsonable(snapshot)
                context.persist_summary()

            if args.command == "preflight":
                if args.json:
                    print(json.dumps(to_jsonable(snapshot), indent=2, sort_keys=True))
                else:
                    print(render_preflight(snapshot))
                return 0 if snapshot.ready else 3

            if args.command == "dependencies":
                return _run_dependencies(args, paths, context, snapshot, runner, transaction_id)

            registry = load_manifest(_SOURCE_ROOT / "components")
            plan = InstallationPlanner(
                paths=paths,
                source_root=_SOURCE_ROOT,
                snapshot=snapshot,
                registry=registry,
                runner=runner,
                transaction_id=transaction_id,
            ).build()

            if context is not None:
                context.transaction.metadata["plan_digest"] = plan.plan_digest
                context.transaction.metadata["manifest_digest"] = plan.manifest_digest
                context.transaction.transition(TransactionState.PLANNED)
                context.persist_json("plan.json", plan)
                context.persist_summary()

            if args.command == "diagnose-current":
                verification = None
                if plan.ready:
                    verification = VerificationEngine(
                        plan=plan,
                        registry=registry,
                        runner=runner,
                        paths=paths,
                        source_root=_SOURCE_ROOT,
                        capability_results=snapshot.capabilities,
                    ).run()
                diagnostic = DiagnosticReportBuilder(
                    paths=paths,
                    source_root=_SOURCE_ROOT,
                    transaction_id=transaction_id,
                    snapshot=snapshot,
                    plan=plan,
                    verification=verification,
                ).build()
                bundle = None
                if not args.dry_run:
                    bundle = report_store.save(diagnostic, output_dir=args.report_path)
                if args.json:
                    print(render_json_report(diagnostic), end="")
                else:
                    print(render_markdown_report(diagnostic), end="")
                    if bundle is not None:
                        print(f"\nSaved diagnostic bundle: {bundle.directory}")
                    else:
                        print("\nDry-run: diagnostic bundle was not persisted.")
                    print("Observation only: no Realmheart installation/configuration changes were made.")
                return 14 if diagnostic.has_failures else 0

            if args.command == "component-plan":
                specs = resolve_component_handler_specs(plan, registry)
                if args.json:
                    print(json.dumps(to_jsonable(specs), indent=2, sort_keys=True))
                else:
                    print(render_component_footprints(specs))
                    print("\nInspection only: no component mutations were executed.")
                return 0 if plan.ready else 12

            if args.command == "verify-current":
                if not plan.ready:
                    if args.json:
                        print(json.dumps({"plan": to_jsonable(plan), "verification": None}, indent=2, sort_keys=True))
                    else:
                        print(render_installation_plan(plan, dry_run=True, verbose=args.verbose))
                        print("\nCurrent-state verification refused because the authoritative plan is blocked.")
                    return 12
                report = VerificationEngine(
                    plan=plan,
                    registry=registry,
                    runner=runner,
                    paths=paths,
                    source_root=_SOURCE_ROOT,
                    capability_results=snapshot.capabilities,
                ).run()
                if args.json:
                    print(json.dumps(to_jsonable(report), indent=2, sort_keys=True))
                else:
                    print(render_verification_report(report, verbose=args.verbose))
                    print("\nObservation only: no Realmheart installation/configuration changes were made.")
                return 0 if report.ok else 14

            if args.dry_run:
                if args.json:
                    print(json.dumps(to_jsonable(plan), indent=2, sort_keys=True))
                else:
                    print(render_installation_plan(plan, dry_run=True, verbose=args.verbose))
                return 0 if plan.ready else 12

            if args.command == "install":
                if not plan.ready:
                    _terminalize_without_mutation(context, scope="install", reason="plan_blocked")
                    if args.json:
                        print(json.dumps({"plan": to_jsonable(plan), "live_install": None}, indent=2, sort_keys=True))
                    else:
                        print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                        print("\nLive installation refused because the authoritative plan is blocked.")
                    return 12
                assert context is not None

                package_actions_applied = not bool(plan.package_actions)
                if plan.package_actions:
                    if not args.install_dependencies:
                        _terminalize_without_mutation(context, scope="install", reason="dependency_consent_required")
                        if args.json:
                            print(json.dumps({"plan": to_jsonable(plan), "error": "dependency package actions require --install-dependencies"}, indent=2, sort_keys=True))
                        else:
                            print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                            print("\nLive installation requires the planned dependency package transaction first. Re-run with --install-dependencies after reviewing the plan.")
                        return 10
                    if snapshot.package_manager.kind != "pacman" or not snapshot.package_manager.automatic_dependency_install:
                        raise InstallerError("automatic dependency installation is unavailable for this environment", code="RH_LIVE_PACKAGE_MANAGER_UNAVAILABLE", stage="dependencies")
                    if plan.package_plan.mutation_blockers or plan.package_plan.unresolved:
                        raise InstallerError("dependency package plan is not safely actionable", code="RH_LIVE_PACKAGE_PLAN_BLOCKED", stage="dependencies")
                    live_consent_granted = args.yes
                    if not args.yes:
                        if args.json:
                            _terminalize_without_mutation(context, scope="install", reason="dependency_consent_required")
                            print(json.dumps({"decision_required": "dependency_and_live_install_consent", "hint": "re-run with --yes after reviewing the plan"}, indent=2, sort_keys=True))
                            return 11
                        print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                        if not _confirm("\nInstall the planned dependencies and continue with the live Realmheart installation? [y/N] "):
                            _terminalize_without_mutation(context, scope="install", reason="cancelled_before_live_mutation")
                            print("Installation cancelled before any package or Realmheart mutation.")
                            return 11
                        live_consent_granted = True
                    adapter = PacmanAdapter(runner)
                    context.ensure_recovery_reserve()
                    context.transaction.transition(TransactionState.APPLYING)
                    context.transaction.metadata["package_install_started"] = True
                    context.persist_summary()
                    package_result = adapter.install(plan.package_plan.packages, required_by=package_required_by(plan.package_plan))
                    context.transaction.metadata["package_install"] = to_jsonable(package_result)
                    context.persist_summary()
                    if not package_result.ok:
                        context.transaction.transition(TransactionState.FAILED)
                        context.persist_summary()
                        persist_recovery_report(
                            context, trigger="dependency_install_failure",
                            primary_error=package_result.error or f"package manager exited {package_result.returncode}",
                        )
                        if args.json:
                            print(json.dumps({"package_install": to_jsonable(package_result)}, indent=2, sort_keys=True))
                        else:
                            print(render_install_result(package_result))
                        return 1
                    # A package transaction changes the environment. Re-probe and
                    # rebuild the authoritative plan under the same transaction ID
                    # before any Realmheart backup/live mutation.
                    snapshot = PreflightScanner(paths=paths, source_root=_SOURCE_ROOT, runner=runner).scan()
                    plan = InstallationPlanner(
                        paths=paths, source_root=_SOURCE_ROOT, snapshot=snapshot, registry=registry,
                        runner=runner, transaction_id=transaction_id,
                    ).build()
                    context.transaction.metadata["post_package_preflight"] = to_jsonable(snapshot)
                    context.transaction.metadata["plan_digest"] = plan.plan_digest
                    context.transaction.metadata["manifest_digest"] = plan.manifest_digest
                    context.persist_json("plan.json", plan)
                    context.persist_summary()
                    if not plan.ready or plan.package_actions:
                        context.transaction.transition(TransactionState.FAILED)
                        context.persist_summary()
                        persist_recovery_report(
                            context, trigger="dependency_reprobe_failure",
                            primary_error="dependency re-probe did not converge to a mutation-ready plan",
                        )
                        raise InstallerError("dependency re-probe did not converge to a mutation-ready plan", code="RH_LIVE_PACKAGE_REPROBE_FAILED", stage="dependencies")
                    package_actions_applied = True
                else:
                    live_consent_granted = args.yes

                if not live_consent_granted:
                    if args.json:
                        # Machine-readable live mutation must be explicit; do not
                        # hide an interactive prompt behind --json.
                        _terminalize_without_mutation(context, scope="install", reason="cancelled_before_live_mutation")
                        print(json.dumps({"decision_required": "initial_live_install_consent", "hint": "re-run with --yes after reviewing the plan"}, indent=2, sort_keys=True))
                        return 11
                    print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                    if not _confirm("\nProceed with the live Realmheart installation? [y/N] "):
                        _terminalize_without_mutation(context, scope="install", reason="cancelled_before_live_mutation")
                        print("Installation cancelled before Realmheart mutation.")
                        return 11

                requested_action = FinalAction(args.decision) if args.decision else None

                def select_final(decision):
                    if requested_action is not None:
                        allowed = {item.action for item in decision.options}
                        if requested_action not in allowed:
                            raise InstallerError(
                                f"--decision {requested_action.value} is not valid for the observed final health state",
                                code="RH_FINAL_DECISION_INVALID", stage="finalization",
                            )
                        return requested_action
                    if args.json:
                        # Never implicitly keep degraded/failed state. When no
                        # explicit machine-readable decision is supplied, choose
                        # the safest reversible option if one exists.
                        rollback = next((item.action for item in decision.options if item.action is FinalAction.RESTORE_PREVIOUS), None)
                        if rollback is not None:
                            return rollback
                        baseline = next((item.action for item in decision.options if item.action is FinalAction.RESTORE_BASELINE), None)
                        if baseline is not None:
                            return baseline
                        return FinalAction.KEEP
                    print("\n" + render_final_decision(decision))
                    while True:
                        try:
                            answer = input("Choose final action: ").strip()
                        except EOFError:
                            answer = ""
                        if answer.isdigit():
                            index = int(answer) - 1
                            if 0 <= index < len(decision.options):
                                return decision.options[index].action
                        # EOF/noninteractive failure: prefer transaction rollback
                        # over silently keeping a degraded/critical installation.
                        if answer == "":
                            rollback = next((item.action for item in decision.options if item.action is FinalAction.RESTORE_PREVIOUS), None)
                            if rollback is not None:
                                return rollback
                        print("Enter one of the listed option numbers.")

                result = LiveInstallExecutor(
                    plan=plan, registry=registry, context=context, paths=paths, source_root=_SOURCE_ROOT,
                    runner=runner, snapshot=snapshot, decision_selector=select_final,
                    package_actions_applied=package_actions_applied,
                ).run()
                if args.json:
                    print(json.dumps(to_jsonable(result), indent=2, sort_keys=True))
                else:
                    print(render_build_stage_report(result.build_report, verbose=False))
                    print("")
                    print(render_verification_report(result.verification, verbose=args.verbose))
                    print("")
                    print(render_acceptance_assessment(result.doctor_assessment))
                    print("")
                    print(render_finalization_result(result.finalization))
                    if result.diagnostic_incident_id:
                        print(f"Diagnostic incident: {result.diagnostic_incident_id}")
                return result.finalization.exit_code

            if args.command == "build-stage":
                if not plan.ready:
                    _terminalize_without_mutation(context, scope="build-stage", reason="plan_blocked")
                    if args.json:
                        print(json.dumps({"plan": to_jsonable(plan), "build_stage": None}, indent=2, sort_keys=True))
                    else:
                        print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                        print("\nBuild/stage execution refused because the authoritative plan is blocked.")
                    return 12
                assert context is not None
                context.transaction.transition(TransactionState.APPLYING)
                context.transaction.metadata["scope"] = "phase10_build_stage_only"
                context.transaction.metadata["live_mutation_started"] = False
                context.persist_summary()
                report = NativeBuildExecutor(
                    plan=plan,
                    source_root=_SOURCE_ROOT,
                    registry=registry,
                    runner=runner,
                    installer_cache=paths.installer_cache,
                ).run()
                context.transaction.metadata["build_stage"] = to_jsonable(report)
                context.persist_json("build-stage.json", report)
                context.transaction.transition(TransactionState.COMMITTED if report.ok else TransactionState.FAILED)
                context.persist_summary()
                if args.json:
                    print(json.dumps({"plan": to_jsonable(plan), "build_stage": to_jsonable(report)}, indent=2, sort_keys=True))
                else:
                    print(render_installation_plan(plan, dry_run=False, verbose=False))
                    print("")
                    print(render_build_stage_report(report, verbose=args.verbose))
                return 0 if report.ok else 13

            if args.json:
                print(json.dumps(to_jsonable(plan), indent=2, sort_keys=True))
            else:
                print(render_installation_plan(plan, dry_run=False, verbose=args.verbose))
                print("")
                print("Use the explicit 'install' command to enter the live transaction; default invocation remains plan-only.")
            return 0 if plan.ready else 12
    except KeyboardInterrupt as exc:
        if context is not None and context.transaction.state not in {
            TransactionState.COMMITTED, TransactionState.ROLLED_BACK, TransactionState.ROLLBACK_FAILED, TransactionState.INTERRUPTED
        }:
            context.transaction.transition(TransactionState.INTERRUPTED)
            context.transaction.metadata["interruption"] = interruption_reason(exc)
            try:
                context.persist_summary()
            except Exception:
                pass
            persist_recovery_report(context, trigger="cli_interruption", primary_error=exc)
        print("Realmheart transaction interrupted. Recovery state was preserved.", file=sys.stderr)
        return 23
    except InstallerError as exc:
        if context is not None and context.transaction.state not in {
            TransactionState.COMMITTED,
            TransactionState.ROLLED_BACK,
            TransactionState.ROLLBACK_FAILED,
            TransactionState.INTERRUPTED,
            TransactionState.FAILED,
        }:
            external_package_action = bool(context.transaction.metadata.get("package_install_started"))
            live_mutation_started = bool(context.transaction.metadata.get("live_mutation_started"))
            if not external_package_action and not live_mutation_started:
                try:
                    context.transaction.metadata["pre_mutation_error"] = {
                        "code": exc.code,
                        "stage": exc.stage,
                    }
                    context.transaction.transition(TransactionState.FAILED)
                    context.persist_summary()
                    context.release_recovery_reserve()
                except Exception:
                    # The original structured installer error remains primary.
                    pass
        print(f"ERROR [{exc.code}]: {exc.message}", file=sys.stderr)
        _persist_terminal_failure_report(
            args=args, exc=exc, paths=paths, transaction_id=transaction_id,
            snapshot=snapshot, plan=plan,
        )
        return 2
    except Exception as exc:
        # Public CLI boundary: unexpected OS/runtime failures must not dump a
        # Python traceback at users or leave a newly-created transaction looking
        # abandoned. Transaction executors already persist detailed recovery
        # state for failures after mutation; this boundary handles startup/build
        # and other failures outside those executors.
        if context is not None and context.transaction.state not in {
            TransactionState.COMMITTED,
            TransactionState.ROLLED_BACK,
            TransactionState.ROLLBACK_FAILED,
            TransactionState.INTERRUPTED,
            TransactionState.FAILED,
        }:
            external_package_action = bool(context.transaction.metadata.get("package_install_started"))
            live_mutation_started = bool(context.transaction.metadata.get("live_mutation_started"))
            try:
                if external_package_action or live_mutation_started:
                    persist_recovery_report(context, trigger="cli_unexpected_exception", primary_error=exc)
                else:
                    context.transaction.metadata["unexpected_pre_mutation_error"] = type(exc).__name__
                    context.transaction.transition(TransactionState.FAILED)
                    context.persist_summary()
                    context.release_recovery_reserve()
            except Exception:
                pass
        print(f"ERROR [RH_UNEXPECTED_FAILURE]: {type(exc).__name__}: {exc}", file=sys.stderr)
        _persist_terminal_failure_report(
            args=args, exc=exc, paths=paths, transaction_id=transaction_id,
            snapshot=snapshot, plan=plan,
        )
        return 1
    finally:
        _restore_termination_signal_handler(previous_sigterm_handler)


def _render_recovery_candidates(candidates) -> str:
    lines = ["Realmheart transaction recovery"]
    if not candidates:
        lines.append("  No unfinished transaction requires recovery.")
        return "\n".join(lines)
    for item in candidates:
        lines.extend([
            "",
            f"  Transaction ......... {item.transaction_id}",
            f"  State ............... {item.transaction_state}",
            f"  Recovery ............ {item.status.value}",
            f"  Automatic rollback .. {'YES' if item.automatic_rollback_safe else 'NO'}",
            f"  Blocks new install .. {'YES' if item.blocks_new_transaction else 'NO'}",
            f"  Report .............. {item.recovery_report}",
        ])
        if item.load_error:
            lines.append(f"  Load error .......... {item.load_error}")
    lines.append("")
    lines.append("Inspect a transaction with: recovery-inspect --transaction-id <ID>")
    lines.append("Manual incidents can be closed after operator repair with: recovery-acknowledge --transaction-id <ID> --yes")
    return "\n".join(lines)


def _render_recovery_report(report) -> str:
    lines = [
        "Realmheart recovery report",
        "",
        f"  Transaction ......... {report.transaction_id}",
        f"  State ............... {report.transaction_state}",
        f"  Status .............. {report.status.value}",
        f"  Automatic rollback .. {'YES' if report.automatic_rollback_safe else 'NO'}",
        f"  Incomplete ops ...... {report.incomplete_operation_count}",
    ]
    if report.journal_error:
        lines.append(f"  Journal error ....... {report.journal_error}")
    if report.primary_error:
        lines.append(f"  Primary error ....... {report.primary_error}")
    if report.rollback_errors:
        lines.append("  Rollback errors:")
        lines.extend(f"    - {item}" for item in report.rollback_errors)
    lines.extend(["", f"  {report.note}"])
    return "\n".join(lines)


def _select_uninstall_config(plan) -> UninstallConfigAction:
    if not plan.baseline_valid:
        print("No valid pre-Realmheart baseline is available; current configuration will be kept.")
        return UninstallConfigAction.KEEP_CURRENT
    while True:
        print("Realmheart configuration backups found.")
        print("Your current configuration may contain changes made after Realmheart was installed.")
        print("")
        print("  [1] Restore pre-Realmheart configuration")
        print("  [2] Keep current configuration")
        print("  [3] Compare before restoring")
        try:
            answer = input("Choose configuration action: ").strip()
        except EOFError:
            answer = ""
        if answer == "1":
            return UninstallConfigAction.RESTORE_BASELINE
        if answer == "2" or answer == "":
            # EOF/noninteractive fallback is the least destructive config choice.
            return UninstallConfigAction.KEEP_CURRENT
        if answer == "3":
            print("")
            print(render_uninstall_compare(plan))
            print("")
            continue
        print("Enter 1, 2, or 3.")


def _confirm(prompt: str) -> bool:
    try:
        answer = input(prompt).strip().lower()
    except EOFError:
        return False
    return answer in {"y", "yes"}


def _terminalize_without_mutation(
    context: InstallContext | None,
    *,
    scope: str,
    reason: str,
    success: bool = False,
) -> None:
    """Close a mutating-intent transaction that never reached mutation.

    Keeping these attempts terminal prevents plan/consent failures from looking
    like abandoned work while retaining useful local provenance about why the
    requested operation did not proceed.
    """

    if context is None:
        return
    context.transaction.metadata["scope"] = scope
    context.transaction.metadata["live_mutation_started"] = False
    context.transaction.metadata[reason] = True
    context.transaction.transition(TransactionState.COMMITTED if success else TransactionState.FAILED)
    context.persist_summary()
    context.release_recovery_reserve()


def _run_dependencies(args, paths, context, snapshot, runner, transaction_id: str) -> int:
    if snapshot.package_manager.kind != "pacman" or not snapshot.package_manager.automatic_dependency_install:
        missing = [item for item in snapshot.capabilities if not item.satisfied and item.requirement.value != "soft"]
        payload = {
            "preflight": to_jsonable(snapshot),
            "package_manager": snapshot.package_manager.kind,
            "automatic_dependency_install": False,
            "missing": [to_jsonable(item) for item in missing],
        }
        if args.json:
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            print(render_preflight(snapshot))
            print("Automatic dependency installation is unavailable for this distro/package-manager combination.")
            if missing:
                print("Unsatisfied capabilities:")
                for item in missing:
                    print(f"  - {item.capability_id}: {item.detail}")
            else:
                print("All required/component capabilities are already satisfied; no package action is needed.")
        _terminalize_without_mutation(
            context,
            scope="dependencies",
            reason="automatic_dependency_install_unavailable" if missing else "no_dependency_mutation_needed",
            success=not missing,
        )
        return 10 if missing else 0

    adapter = PacmanAdapter(runner)
    registry = load_manifest(Path(snapshot.installation.source.source_root) / "components")
    package_plan = build_pacman_dependency_plan(snapshot.capabilities, adapter, registry=registry)
    if context is not None:
        context.transaction.metadata["package_plan"] = to_jsonable(package_plan)
        context.transaction.transition(TransactionState.PLANNED)
        context.persist_summary()

    if args.json and (not args.install_dependencies or args.dry_run):
        print(json.dumps({"preflight": to_jsonable(snapshot), "package_plan": to_jsonable(package_plan)}, indent=2, sort_keys=True))
    elif not args.json:
        print(render_preflight(snapshot))
        print("")
        if context is not None:
            print(f"Transaction: {transaction_id}")
        print(render_package_plan(package_plan))

    if not args.install_dependencies or not package_plan.packages or args.dry_run:
        if args.dry_run and args.install_dependencies and package_plan.packages and not args.json:
            print("\nDry-run: package mutation suppressed.")
        success = not package_plan.unresolved and not package_plan.mutation_blockers
        if context is not None:
            _terminalize_without_mutation(
                context,
                scope="dependencies",
                reason="no_dependency_mutation_needed" if success else "dependency_plan_blocked",
                success=success,
            )
        return 0 if success else 10
    if package_plan.mutation_blockers:
        _terminalize_without_mutation(context, scope="dependencies", reason="dependency_plan_blocked")
        if not args.json:
            print("Refusing package mutation: " + "; ".join(package_plan.mutation_blockers))
        return 10
    if package_plan.unresolved:
        _terminalize_without_mutation(context, scope="dependencies", reason="dependency_plan_unresolved")
        if not args.json:
            print("Refusing package mutation while mandatory capabilities remain unresolved by the verified adapter.")
        return 10
    if not args.yes:
        if args.json:
            _terminalize_without_mutation(context, scope="dependencies", reason="dependency_consent_required")
            print(json.dumps({"decision_required": "dependency_install_consent", "hint": "re-run with --yes after reviewing the package plan"}, indent=2, sort_keys=True))
            return 11
        try:
            answer = input("Install the proposed packages with pacman? [y/N] ").strip().lower()
        except EOFError:
            answer = ""
        if answer not in {"y", "yes"}:
            _terminalize_without_mutation(context, scope="dependencies", reason="cancelled_before_package_mutation")
            if not args.json:
                print("Dependency installation cancelled before mutation.")
            return 11

    assert context is not None  # dry-run returned above
    context.ensure_recovery_reserve()
    context.transaction.transition(TransactionState.APPLYING)
    context.transaction.metadata["package_install_started"] = True
    context.persist_summary()
    result = adapter.install(package_plan.packages, required_by=package_required_by(package_plan))
    context.transaction.metadata["package_install"] = to_jsonable(result)
    if not result.ok:
        context.transaction.transition(TransactionState.FAILED)
    context.persist_summary()
    if not result.ok:
        persist_recovery_report(
            context, trigger="dependency_install_failure",
            primary_error=result.error or f"package manager exited {result.returncode}",
        )
    if not args.json:
        print("")
        print(render_install_result(result))
    if not result.ok:
        if args.json:
            print(json.dumps(to_jsonable(result), indent=2, sort_keys=True))
        return 1

    context.transaction.transition(TransactionState.VERIFYING)
    context.persist_summary()
    verified = PreflightScanner(paths=paths, source_root=Path(snapshot.installation.source.source_root), runner=runner).scan()
    originally_missing = {item.capability_id for item in snapshot.capabilities if not item.satisfied and item.requirement.value != "soft"}
    still_missing = [item for item in verified.capabilities if item.capability_id in originally_missing and not item.satisfied]
    context.transaction.metadata["post_package_preflight"] = to_jsonable(verified)
    context.persist_summary()
    if still_missing:
        context.transaction.transition(TransactionState.FAILED)
        context.persist_summary()
        persist_recovery_report(
            context, trigger="dependency_reprobe_failure",
            primary_error="package transaction completed but required capabilities still fail direct verification",
        )
        if args.json:
            print(json.dumps({"package_install": to_jsonable(result), "post_package_preflight": to_jsonable(verified)}, indent=2, sort_keys=True))
        else:
            print("\nPackage transaction completed, but direct capability verification still fails:")
            for item in still_missing:
                print(f"  - {item.capability_id}: {item.detail}")
        return 1
    context.transaction.transition(TransactionState.COMMITTED)
    context.persist_summary()
    context.release_recovery_reserve()
    if args.json:
        print(json.dumps({"package_install": to_jsonable(result), "post_package_preflight": to_jsonable(verified)}, indent=2, sort_keys=True))
    else:
        print("\nDirect capability re-probe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
