"""Private persistence + inspect/remove workflow for diagnostic bundles."""
from __future__ import annotations

import json
import os
import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from ..context import XdgPaths
from ..durability import fsync_directory
from ..errors import InstallerError
from .models import DiagnosticReport
from .render import render_github_issue, render_json_report, render_markdown_report

_REPORT_ID_RE = re.compile(r"^RH-DIAG-\d{8}-\d{6}-[A-F0-9]{8}-[A-F0-9]{4}$")
_ALLOWED_FILES = {"report.json", "report.md", "github-issue.md"}


@dataclass(frozen=True)
class DiagnosticBundle:
    incident_id: str
    directory: Path
    json_path: Path
    markdown_path: Path
    github_path: Path


class DiagnosticReportStore:
    def __init__(self, paths: XdgPaths) -> None:
        self.paths = paths

    def save(self, report: DiagnosticReport, *, output_dir: Path | None = None) -> DiagnosticBundle:
        base = Path(output_dir).expanduser() if output_dir is not None else self.paths.reports
        if base.exists() and (base.is_symlink() or not base.is_dir()):
            raise InstallerError("Diagnostic report output root is not a trusted directory.", code="RH_DIAGNOSTIC_STORE_UNSAFE", stage="diagnostics")
        base.mkdir(parents=True, exist_ok=True, mode=0o700)
        directory = base / report.incident_id
        directory.mkdir(parents=False, exist_ok=False, mode=0o700)
        bundle = DiagnosticBundle(report.incident_id, directory, directory / "report.json", directory / "report.md", directory / "github-issue.md")
        try:
            self._atomic_write(bundle.json_path, render_json_report(report))
            self._atomic_write(bundle.markdown_path, render_markdown_report(report))
            self._atomic_write(bundle.github_path, render_github_issue(report))
        except Exception:
            shutil.rmtree(directory, ignore_errors=True)
            raise
        return bundle

    def list(self) -> tuple[DiagnosticBundle, ...]:
        root = self.paths.reports
        if not root.exists():
            return ()
        if root.is_symlink() or not root.is_dir():
            raise InstallerError("Diagnostic reports root is not a trusted directory.", code="RH_DIAGNOSTIC_STORE_UNSAFE", stage="diagnostics")
        bundles: list[DiagnosticBundle] = []
        for item in sorted(root.iterdir(), reverse=True):
            if item.is_symlink() or not item.is_dir() or not _REPORT_ID_RE.fullmatch(item.name):
                continue
            bundles.append(DiagnosticBundle(item.name, item, item / "report.json", item / "report.md", item / "github-issue.md"))
        return tuple(bundles)

    def inspect(self, incident_id: str) -> tuple[DiagnosticBundle, dict[str, object], str]:
        bundle = self._bundle(incident_id)
        if not bundle.json_path.is_file() or bundle.json_path.is_symlink():
            raise InstallerError("Diagnostic JSON report is missing or unsafe.", code="RH_DIAGNOSTIC_REPORT_INVALID", stage="diagnostics")
        try:
            payload = json.loads(bundle.json_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise InstallerError("Diagnostic JSON report is unreadable/corrupt.", code="RH_DIAGNOSTIC_REPORT_INVALID", stage="diagnostics") from exc
        if not isinstance(payload, dict) or payload.get("incident_id") != incident_id or payload.get("schema_version") != 1:
            raise InstallerError("Diagnostic report identity/schema mismatch.", code="RH_DIAGNOSTIC_REPORT_INVALID", stage="diagnostics")
        markdown = bundle.markdown_path.read_text(encoding="utf-8") if bundle.markdown_path.is_file() and not bundle.markdown_path.is_symlink() else ""
        return bundle, payload, markdown

    def remove(self, incident_id: str) -> None:
        bundle = self._bundle(incident_id)
        entries = set()
        try:
            for item in bundle.directory.iterdir():
                if item.is_symlink() or not item.is_file():
                    raise InstallerError("Refusing to remove diagnostic bundle containing non-regular files.", code="RH_DIAGNOSTIC_STORE_UNSAFE", stage="diagnostics")
                entries.add(item.name)
        except OSError as exc:
            raise InstallerError("Cannot inspect diagnostic bundle before removal.", code="RH_DIAGNOSTIC_REPORT_INVALID", stage="diagnostics") from exc
        unexpected = entries - _ALLOWED_FILES
        if unexpected:
            raise InstallerError("Refusing to remove diagnostic bundle with unexpected files.", code="RH_DIAGNOSTIC_STORE_UNSAFE", stage="diagnostics")
        for name in _ALLOWED_FILES:
            target = bundle.directory / name
            if target.exists():
                target.unlink()
        bundle.directory.rmdir()
        fsync_directory(self.paths.reports)

    def _bundle(self, incident_id: str) -> DiagnosticBundle:
        if not _REPORT_ID_RE.fullmatch(incident_id):
            raise InstallerError("Invalid diagnostic incident id.", code="RH_DIAGNOSTIC_ID_INVALID", stage="diagnostics")
        root = self.paths.reports
        directory = root / incident_id
        if directory.is_symlink() or not directory.is_dir():
            raise InstallerError("Diagnostic incident was not found.", code="RH_DIAGNOSTIC_NOT_FOUND", stage="diagnostics")
        return DiagnosticBundle(incident_id, directory, directory / "report.json", directory / "report.md", directory / "github-issue.md")

    @staticmethod
    def _atomic_write(target: Path, text: str) -> None:
        temporary = target.with_name(f".{target.name}.tmp")
        with temporary.open("w", encoding="utf-8") as handle:
            os.chmod(temporary, 0o600)
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, target)
        fsync_directory(target.parent)
