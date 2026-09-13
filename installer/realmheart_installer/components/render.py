"""Human-readable Phase-12 component progress/report rendering."""
from __future__ import annotations

from ..models import ComponentState
from .models import ComponentExecutionReport, ComponentHandlerSpec, ComponentProgressEvent


def render_progress_event(event: ComponentProgressEvent) -> str:
    detail = f" — {event.detail}" if event.detail else ""
    return (
        f"[{event.index:02d}/{event.total:02d}] {event.display_name} "
        f"[{event.category}] {event.state.upper()}{detail}"
    )


def render_component_report(report: ComponentExecutionReport) -> str:
    lines = ["Realmheart component execution"]
    final_events: dict[str, ComponentProgressEvent] = {}
    for event in report.progress:
        if event.state != ComponentState.RUNNING.value:
            final_events[event.component_id] = event
    for result in report.results:
        event = final_events.get(result.component_id)
        if event is not None:
            lines.append(render_progress_event(event))
    passed = sum(item.state is ComponentState.PASS for item in report.results)
    failed = sum(item.state is ComponentState.FAILED for item in report.results)
    blocked = sum(item.state is ComponentState.BLOCKED for item in report.results)
    warning = sum(item.state is ComponentState.WARNING for item in report.results)
    skipped = sum(item.state is ComponentState.SKIPPED for item in report.results)
    lines.extend((
        "",
        f"PASS={passed} FAILED={failed} BLOCKED={blocked} WARNING={warning} SKIPPED={skipped}",
    ))
    return "\n".join(lines)


def render_component_footprints(specs: tuple[ComponentHandlerSpec, ...]) -> str:
    lines = ["Realmheart component handler footprints"]
    for index, spec in enumerate(specs, start=1):
        fp = spec.footprint
        lines.append(
            f"[{index:02d}/{len(specs):02d}] {spec.component.name}: "
            f"artifacts={len(fp.artifact_ids)} config={len(fp.config_action_ids)} "
            f"services={len(fp.service_action_ids)} checks={len(fp.health_check_ids)} "
            f"rollback={len(fp.rollback_requirements)}"
        )
    return "\n".join(lines)
