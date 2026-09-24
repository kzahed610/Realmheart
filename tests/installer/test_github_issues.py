from __future__ import annotations

import io
import unittest
from urllib.parse import parse_qs, urlsplit

from . import _bootstrap  # noqa: F401
from realmheart_maintenance.github_issues import (
    MAX_PREFILL_BODY_CHARS,
    build_github_issue_url,
    offer_github_issue,
)


class _TTY(io.StringIO):
    def isatty(self) -> bool:
        return True


class _Pipe(io.StringIO):
    def isatty(self) -> bool:
        return False


class GitHubIssueHandoffTests(unittest.TestCase):
    def test_prefill_url_contains_machine_authored_title_and_body(self) -> None:
        url, truncated = build_github_issue_url("[Installer] RH_TEST", "safe body")
        query = parse_qs(urlsplit(url).query)
        self.assertFalse(truncated)
        self.assertEqual(query["title"], ["[Installer] RH_TEST"])
        self.assertEqual(query["body"], ["safe body"])

    def test_long_body_is_bounded_and_explains_local_authority(self) -> None:
        url, truncated = build_github_issue_url("Realmheart", "x" * (MAX_PREFILL_BODY_CHARS * 2))
        query = parse_qs(urlsplit(url).query)
        self.assertTrue(truncated)
        self.assertLessEqual(len(query["body"][0]), MAX_PREFILL_BODY_CHARS)
        self.assertIn("complete machine-authored report remains", query["body"][0])

    def test_noninteractive_handoff_never_prompts_or_opens(self) -> None:
        opened = []
        result = offer_github_issue(
            "Realmheart", "safe", stdin=_Pipe(), stderr=io.StringIO(),
            opener=lambda *args, **kwargs: opened.append(args) or True,
        )
        self.assertIsNone(result)
        self.assertEqual(opened, [])

    def test_interactive_handoff_requires_explicit_yes(self) -> None:
        opened = []
        declined = offer_github_issue(
            "Realmheart", "safe", stdin=_TTY("\n"), stderr=io.StringIO(),
            opener=lambda *args, **kwargs: opened.append(args) or True,
        )
        self.assertIsNone(declined)
        self.assertEqual(opened, [])
        accepted = offer_github_issue(
            "Realmheart", "safe", stdin=_TTY("yes\n"), stderr=io.StringIO(),
            opener=lambda *args, **kwargs: opened.append(args) or True,
        )
        self.assertTrue(accepted.opened)
        self.assertEqual(len(opened), 1)


if __name__ == "__main__":
    unittest.main()
