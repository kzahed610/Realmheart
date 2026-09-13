from __future__ import annotations

import subprocess
import unittest
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.environment.command import CommandRunner


class CommandRunnerTests(unittest.TestCase):
    def test_observational_commands_are_noninteractive_and_captured(self):
        completed = subprocess.CompletedProcess(("probe",), 0, stdout="ok\n", stderr="")
        with patch("realmheart_installer.environment.command.subprocess.run", return_value=completed) as run:
            result = CommandRunner(env={"PATH":"/usr/bin"}).run(("probe",))
        kwargs = run.call_args.kwargs
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stdout"], subprocess.PIPE)
        self.assertEqual(result.stdout, "ok\n")

    def test_interactive_mutation_inherits_terminal_streams(self):
        completed = subprocess.CompletedProcess(("sudo","pacman"), 0, stdout=None, stderr=None)
        with patch("realmheart_installer.environment.command.subprocess.run", return_value=completed) as run:
            result = CommandRunner(env={"PATH":"/usr/bin"}).run(("sudo","pacman"), timeout=None, interactive=True)
        kwargs = run.call_args.kwargs
        self.assertIsNone(kwargs["stdin"])
        self.assertIsNone(kwargs["stdout"])
        self.assertIsNone(kwargs["stderr"])
        self.assertTrue(result.ok)


if __name__ == "__main__":
    unittest.main()
