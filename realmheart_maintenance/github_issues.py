"""Consent-gated browser handoff for machine-authored Realmheart GitHub issues.

This module never submits an issue.  It only constructs a bounded pre-filled
GitHub issue-editor URL and, after explicit interactive consent, asks the user's
browser to open it.  Callers must pass already-public-safe report text.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys
import webbrowser
from urllib.parse import urlencode

from .redaction import sanitize_text, scan_secrets

REALMHEART_NEW_ISSUE_URL = "https://github.com/kzahed610/Realmheart/issues/new"
# Keep the complete URL comfortably below common proxy/browser request-line
# limits.  The authoritative report always remains on disk.
MAX_PREFILL_BODY_CHARS = 5_500
MAX_PREFILL_TITLE_CHARS = 180


@dataclass(frozen=True)
class GitHubIssueHandoff:
    opened: bool
    url: str
    body_truncated: bool


def _bounded_title(title: str) -> str:
    compact = " ".join(sanitize_text(str(title)).split())
    return compact[:MAX_PREFILL_TITLE_CHARS] or "Realmheart incident"


def _bounded_body(body: str) -> tuple[str, bool]:
    text = sanitize_text(str(body)).replace("\r\n", "\n").replace("\r", "\n")
    findings = scan_secrets(text)
    if findings:
        text = (
            "Realmheart withheld the browser prefill because the residual secret scanner "
            f"requested local review ({', '.join(findings)}). Use the local machine-authored "
            "report after reviewing it manually."
        )
    if len(text) <= MAX_PREFILL_BODY_CHARS:
        return text, False
    suffix = (
        "\n\n---\n"
        "_This browser prefill was truncated to keep the GitHub URL bounded. "
        "The complete machine-authored report remains in the local incident bundle._\n"
    )
    keep = max(0, MAX_PREFILL_BODY_CHARS - len(suffix))
    return text[:keep].rstrip() + suffix, True


def build_github_issue_url(title: str, body: str) -> tuple[str, bool]:
    """Return a bounded GitHub new-issue URL and whether the body was truncated."""
    bounded_body, truncated = _bounded_body(body)
    query = urlencode({"title": _bounded_title(title), "body": bounded_body})
    return f"{REALMHEART_NEW_ISSUE_URL}?{query}", truncated


def open_github_issue(title: str, body: str, *, opener=webbrowser.open) -> GitHubIssueHandoff:
    """Open a pre-filled editor.  This never authenticates or submits an issue."""
    url, truncated = build_github_issue_url(title, body)
    try:
        opened = bool(opener(url, new=2))
    except Exception:
        opened = False
    return GitHubIssueHandoff(opened=opened, url=url, body_truncated=truncated)


def offer_github_issue(
    title: str,
    body: str,
    *,
    report_path: Path | None = None,
    stdin=None,
    stderr=None,
    opener=webbrowser.open,
) -> GitHubIssueHandoff | None:
    """Ask before transmitting report text to GitHub via the browser URL.

    Non-interactive callers never block and never open a browser.  An explicit
    ``y``/``yes`` is required because opening the URL transmits the pre-filled
    issue body to GitHub even though the user still performs final submission.
    """
    stdin = sys.stdin if stdin is None else stdin
    stderr = sys.stderr if stderr is None else stderr
    if not getattr(stdin, "isatty", lambda: False)():
        return None
    if report_path is not None:
        print(f"Machine-authored public report: {report_path}", file=stderr)
    print(
        "Open a pre-filled GitHub issue in your browser? "
        "The report body will be sent to GitHub for review, but nothing is submitted automatically. [y/N] ",
        end="",
        file=stderr,
        flush=True,
    )
    try:
        answer = stdin.readline()
    except (OSError, KeyboardInterrupt):
        return None
    if answer.strip().lower() not in {"y", "yes"}:
        return None
    result = open_github_issue(title, body, opener=opener)
    if result.opened:
        print("Opened the GitHub issue editor. Review the generated report before submitting.", file=stderr)
        if result.body_truncated and report_path is not None:
            print(f"Browser prefill was shortened; the complete report is at {report_path}", file=stderr)
    else:
        print("Could not open a browser. The machine-authored report remains available locally.", file=stderr)
    return result
