"""Repair runners execute bounded structured argv and never trust an exit code."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from realmheart_doctor.repair import (
    ACTION_INSTALL_PACKAGE,
    ACTION_REBUILD,
    ACTION_RESTART_SERVICE,
    RepairAction,
)
from realmheart_doctor.repair_runners import PackageInstallRunner, RebuildRunner, ServiceRestartRunner


class _Result:
    def __init__(self, returncode: int, stdout: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = ""


def _install_action(**kwargs) -> RepairAction:
    return RepairAction(ACTION_INSTALL_PACKAGE, "PRIVILEGED_CONFIRM", "install", **kwargs)


class PackageInstallRunnerTests(unittest.TestCase):
    def test_non_interactive_install_is_refused(self):
        runner = PackageInstallRunner(stdin_isatty=lambda: False,
                                      runner=lambda argv, timeout: self.fail("must not run"))
        outcome = runner(_install_action(packages=("grim",)))
        self.assertEqual(outcome.status, "refused")

    def test_install_uses_structured_argv_and_requeries(self):
        calls: list[tuple[str, ...]] = []

        def fake(argv, timeout):
            calls.append(tuple(argv))
            if "-Q" in argv:
                return _Result(0, f"{argv[-1]} 1.0-1\n")
            return _Result(0)

        runner = PackageInstallRunner(stdin_isatty=lambda: True, runner=fake,
                                      sudo="sudo", pacman="pacman")
        outcome = runner(_install_action(packages=("grim", "slurp")))
        self.assertEqual(outcome.status, "succeeded")
        self.assertEqual(calls[0], ("sudo", "pacman", "-S", "--needed", "grim", "slurp"))
        self.assertEqual([call[-1] for call in calls[1:]], ["grim", "slurp"])

    def test_successful_exit_without_an_installed_package_is_a_failure(self):
        def fake(argv, timeout):
            if "-Q" in argv:
                return _Result(1)
            return _Result(0)

        runner = PackageInstallRunner(stdin_isatty=lambda: True, runner=fake)
        outcome = runner(_install_action(packages=("grim",)))
        self.assertEqual(outcome.status, "failed")
        self.assertIn("grim", outcome.detail)

    def test_failed_install_is_reported(self):
        runner = PackageInstallRunner(stdin_isatty=lambda: True,
                                      runner=lambda argv, timeout: _Result(1))
        outcome = runner(_install_action(packages=("grim",)))
        self.assertEqual(outcome.status, "failed")


class RebuildRunnerTests(unittest.TestCase):
    def test_missing_build_directory_is_unavailable(self):
        runner = RebuildRunner(component_id="screenshot")
        outcome = runner(RepairAction(ACTION_REBUILD, "CONFIRM", "rebuild", targets=("realmheart_screenshot",)))
        self.assertEqual(outcome.status, "unavailable")

    def test_installer_bound_component_is_refused(self):
        runner = RebuildRunner(component_id="lockscreen-auth", build_dir="/nonexistent", prefix="/tmp",
                               installer_bound=True)
        outcome = runner(RepairAction(ACTION_REBUILD, "CONFIRM", "rebuild", targets=("realmheart_auth_helper",)))
        self.assertEqual(outcome.status, "refused")

    def test_rebuild_builds_then_installs_only_the_component(self):
        calls: list[tuple[str, ...]] = []

        def fake(argv, timeout):
            calls.append(tuple(argv))
            return _Result(0)

        with tempfile.TemporaryDirectory() as temp:
            runner = RebuildRunner(component_id="screenshot", build_dir=temp, prefix="/usr",
                                   runner=fake, cmake="cmake")
            outcome = runner(RepairAction(ACTION_REBUILD, "CONFIRM", "rebuild",
                                          targets=("realmheart_screenshot", "realmheart_screenshot_regions")))
        self.assertEqual(outcome.status, "succeeded")
        self.assertEqual(calls[0], ("cmake", "--build", temp, "--target",
                                    "realmheart_screenshot", "realmheart_screenshot_regions"))
        self.assertEqual(calls[1], ("cmake", "--install", temp, "--component", "screenshot",
                                    "--prefix", "/usr"))

    def test_failed_build_stops_before_install(self):
        calls: list[tuple[str, ...]] = []

        def fake(argv, timeout):
            calls.append(tuple(argv))
            return _Result(1)

        with tempfile.TemporaryDirectory() as temp:
            runner = RebuildRunner(component_id="screenshot", build_dir=temp, prefix="/usr", runner=fake)
            outcome = runner(RepairAction(ACTION_REBUILD, "CONFIRM", "rebuild", targets=("realmheart_screenshot",)))
        self.assertEqual(outcome.status, "failed")
        self.assertEqual(len(calls), 1)


class ServiceRestartRunnerTests(unittest.TestCase):
    def test_restart_uses_the_user_manager(self):
        calls: list[tuple[str, ...]] = []

        def fake(argv, timeout):
            calls.append(tuple(argv))
            return _Result(0)

        runner = ServiceRestartRunner(runner=fake, systemctl="systemctl")
        outcome = runner(RepairAction(ACTION_RESTART_SERVICE, "CONFIRM", "restart",
                                      targets=("realmheart-eventd.service",)))
        self.assertEqual(outcome.status, "succeeded")
        self.assertEqual(calls, [("systemctl", "--user", "restart", "realmheart-eventd.service")])

    def test_one_failed_unit_fails_the_action(self):
        def fake(argv, timeout):
            return _Result(1 if argv[-1] == "b.service" else 0)

        runner = ServiceRestartRunner(runner=fake)
        outcome = runner(RepairAction(ACTION_RESTART_SERVICE, "CONFIRM", "restart",
                                      targets=("a.service", "b.service")))
        self.assertEqual(outcome.status, "failed")
        self.assertIn("b.service", outcome.detail)


if __name__ == "__main__":
    unittest.main()
