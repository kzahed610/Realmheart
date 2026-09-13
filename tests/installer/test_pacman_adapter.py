from __future__ import annotations

import unittest

from . import _bootstrap  # noqa: F401
from realmheart_installer.environment.capabilities import CapabilityResult, CapabilityState, DependencyLifecycle, RequirementLevel
from realmheart_installer.environment.command import CommandResult
from realmheart_installer.package_manager.base import ProviderResolution
from realmheart_installer.package_manager.pacman import PacmanAdapter, build_pacman_dependency_plan, package_required_by


class FakeRunner:
    def __init__(self, *, installed=None, repo=None, sudo=True, pending=(), upgrade_query_error=None) -> None:
        self.installed = dict(installed or {})
        self.repo = dict(repo or {})
        self.commands = []
        self.sudo = sudo
        self.pending = tuple(pending)
        self.upgrade_query_error = upgrade_query_error

    def which(self, executable):
        if executable == "pacman": return "/usr/bin/pacman"
        if executable == "sudo": return "/usr/bin/sudo" if self.sudo else None
        if executable == "vercmp": return "/usr/bin/vercmp"
        return None

    def run(self, argv, **kwargs):
        key = tuple(str(x) for x in argv)
        self.commands.append((key, kwargs))
        if key[:3] == ("/usr/bin/pacman", "-Q", "--"):
            pkg = key[3]
            if pkg in self.installed:
                return CommandResult(key, 0, f"{pkg} {self.installed[pkg]}\n")
            return CommandResult(key, 1, stderr="not installed")
        if key[:3] == ("/usr/bin/pacman", "-Si", "--"):
            pkg = key[3]
            if pkg in self.repo:
                return CommandResult(key, 0, f"Repository      : extra\nName            : {pkg}\nVersion         : {self.repo[pkg]}\n")
            return CommandResult(key, 1, stderr="target not found")
        if key == ("/usr/bin/pacman", "-Qu"):
            if self.upgrade_query_error:
                return CommandResult(key, 2, stderr=self.upgrade_query_error)
            return CommandResult(key, 0, "\n".join(self.pending) + ("\n" if self.pending else ""))
        if key and key[0] == "/usr/bin/vercmp":
            left, right = key[1], key[2]
            # Sufficient deterministic fake for test versions.
            value = -1 if left < right else (1 if left > right else 0)
            return CommandResult(key, 0, str(value) + "\n")
        if key[:4] == ("/usr/bin/sudo", "/usr/bin/pacman", "-S", "--needed"):
            pkgs = key[5:]
            for pkg in pkgs:
                if pkg in self.repo:
                    self.installed[pkg] = self.repo[pkg]
            return CommandResult(key, 0)
        if key[:3] == ("/usr/bin/sudo", "/usr/bin/pacman", "-R"):
            pkgs = key[4:]
            for pkg in pkgs:
                self.installed.pop(pkg, None)
            return CommandResult(key, 0)
        return CommandResult(key, 1, stderr="not mocked")


def missing(cid, name="missing", *, requirement=RequirementLevel.REQUIRED):
    return CapabilityResult(cid, name, CapabilityState.MISSING, requirement, (DependencyLifecycle.RUNTIME,), "missing")


class PacmanAdapterTests(unittest.TestCase):
    def test_query_records_installed_and_sync_versions(self):
        adapter = PacmanAdapter(FakeRunner(installed={"fish":"4.7.1-1"}, repo={"fish":"4.8.1-1"}))
        state = adapter.query("fish")
        self.assertTrue(state.installed)
        self.assertEqual(state.installed_version, "4.7.1-1")
        self.assertEqual(state.repository, "extra")
        self.assertEqual(state.repository_version, "4.8.1-1")

    def test_missing_capability_resolves_to_verified_repo_package(self):
        adapter = PacmanAdapter(FakeRunner(repo={"opencv":"4.13.0-1"}))
        plan = build_pacman_dependency_plan((missing("opencv.ximgproc"),), adapter)
        self.assertEqual(plan.packages, ("opencv",))
        self.assertEqual(plan.providers[0].resolution, ProviderResolution.INSTALL)

    def test_hyprland_is_never_automatic(self):
        adapter = PacmanAdapter(FakeRunner(repo={"hyprland":"0.56.2-1"}))
        plan = build_pacman_dependency_plan((missing("hyprland.devel"),), adapter)
        self.assertFalse(plan.packages)
        self.assertEqual(plan.providers[0].resolution, ProviderResolution.MANUAL)
        self.assertIn("does not", plan.providers[0].reason.lower())

    def test_installed_provider_without_upgrade_is_manual_not_reinstall(self):
        adapter = PacmanAdapter(FakeRunner(installed={"networkmanager":"1.54.0-1"}, repo={"networkmanager":"1.54.0-1"}))
        plan = build_pacman_dependency_plan((missing("runtime.nmcli", requirement=RequirementLevel.COMPONENT),), adapter)
        self.assertFalse(plan.packages)
        self.assertEqual(plan.providers[0].resolution, ProviderResolution.MANUAL)

    def test_outdated_provider_is_upgrade_candidate(self):
        adapter = PacmanAdapter(FakeRunner(installed={"fish":"4.7.1-1"}, repo={"fish":"4.8.1-1"}))
        plan = build_pacman_dependency_plan((missing("runtime.fish", requirement=RequirementLevel.COMPONENT),), adapter)
        self.assertEqual(plan.packages, ("fish",))
        self.assertEqual(plan.providers[0].resolution, ProviderResolution.UPGRADE)

    def test_repo_missing_provider_is_unavailable(self):
        adapter = PacmanAdapter(FakeRunner())
        plan = build_pacman_dependency_plan((missing("runtime.matugen", requirement=RequirementLevel.COMPONENT),), adapter)
        self.assertEqual(plan.providers[0].resolution, ProviderResolution.UNAVAILABLE)
        self.assertFalse(plan.actionable)

    def test_install_uses_single_needed_transaction_and_records_provenance(self):
        runner = FakeRunner(repo={"opencv":"4.13.0-1", "tesseract":"5.5.3-1"})
        adapter = PacmanAdapter(runner)
        result = adapter.install(("opencv","tesseract"), required_by={"opencv":("opencv.ximgproc",), "tesseract":("runtime.tesseract",)})
        self.assertTrue(result.ok)
        self.assertEqual(result.command, ("/usr/bin/sudo","/usr/bin/pacman","-S","--needed","--","opencv","tesseract"))
        self.assertTrue(all(item.installed_by_transaction for item in result.provenance))
        mutation_calls = [call for call, kwargs in runner.commands if call[:2] == ("/usr/bin/sudo","/usr/bin/pacman")]
        self.assertEqual(len(mutation_calls), 1)

    def test_install_without_sudo_fails_before_mutation(self):
        runner = FakeRunner(repo={"opencv":"4.13.0-1"}, sudo=False)
        result = PacmanAdapter(runner).install(("opencv",))
        self.assertFalse(result.ok)
        self.assertEqual(result.returncode, 127)
        self.assertIn("sudo", result.error or "")

    def test_package_required_by_deduplicates_capability_reasons(self):
        adapter = PacmanAdapter(FakeRunner(repo={"wl-clipboard":"2.2.1-1"}))
        plan = build_pacman_dependency_plan((
            missing("runtime.wl-copy", requirement=RequirementLevel.COMPONENT),
            missing("runtime.wl-paste", requirement=RequirementLevel.COMPONENT),
        ), adapter)
        self.assertEqual(plan.packages, ("wl-clipboard",))
        self.assertEqual(package_required_by(plan)["wl-clipboard"], ("runtime.wl-copy", "runtime.wl-paste"))

    def test_cleanup_is_intentionally_conservative(self):
        adapter = PacmanAdapter(FakeRunner(installed={"opencv":"4.13.0-1"}, repo={"opencv":"4.13.0-1"}))
        self.assertIsNone(adapter.can_remove_safely("opencv"))

    def test_explicit_cleanup_uses_exact_non_recursive_removal_only(self):
        runner = FakeRunner(installed={"wl-clipboard":"2.2.1-1", "fish":"4.0-1"})
        result = PacmanAdapter(runner).remove_exact(("wl-clipboard",))
        self.assertTrue(result.ok, result.error)
        self.assertEqual(result.removed, ("wl-clipboard",))
        self.assertIn("fish", runner.installed)
        self.assertEqual(result.command, ("/usr/bin/sudo", "/usr/bin/pacman", "-R", "--", "wl-clipboard"))
        self.assertFalse(any(flag in result.command for flag in ("-Rs", "-Rns")))

    def test_pending_system_upgrade_blocks_selected_dependency_install(self):
        runner = FakeRunner(repo={"opencv":"4.13.0-1"}, pending=("glibc 2.42-1 -> 2.43-1",))
        adapter = PacmanAdapter(runner)
        plan = build_pacman_dependency_plan((missing("opencv.ximgproc"),), adapter)
        self.assertTrue(plan.mutation_blockers)
        result = adapter.install(("opencv",))
        self.assertFalse(result.ok)
        self.assertEqual(result.returncode, 75)
        self.assertIn("partial upgrade", (result.error or "").lower())
        self.assertFalse(any(call[:2] == ("/usr/bin/sudo", "/usr/bin/pacman") for call, _ in runner.commands))

    def test_package_name_option_injection_is_refused(self):
        adapter = PacmanAdapter(FakeRunner())
        with self.assertRaises(ValueError):
            adapter.query("--overwrite=*")

    def test_unreadable_upgrade_state_fails_closed(self):
        runner = FakeRunner(repo={"opencv":"4.13.0-1"}, upgrade_query_error="database error")
        adapter = PacmanAdapter(runner)
        plan = build_pacman_dependency_plan((missing("opencv.ximgproc"),), adapter)
        self.assertTrue(plan.mutation_blockers)
        self.assertIn("verify pacman upgrade safety", plan.mutation_blockers[0])
        result = adapter.install(("opencv",))
        self.assertFalse(result.ok)
        self.assertIn("database error", result.error or "")


if __name__ == "__main__":
    unittest.main()

class PacmanMappingCoverageTests(unittest.TestCase):
    def test_all_mandatory_phase5_capabilities_have_pacman_provider_policy(self):
        from realmheart_installer.environment.capabilities import EXECUTABLE_SPECS, PKG_CONFIG_SPECS
        from realmheart_installer.package_manager.pacman import PACMAN_CAPABILITY_PROVIDERS
        ids = {
            spec.capability_id for spec in (*EXECUTABLE_SPECS, *PKG_CONFIG_SPECS)
            if spec.requirement is not RequirementLevel.SOFT
        }
        ids.update({
            "wayland.scanner", "build.cxx26", "opencv.ximgproc", "pam.devel",
            "tesseract.lang.eng", "runtime.nmcli", "runtime.bluetoothctl",
            "runtime.powerprofilesctl", "runtime.systemd-user", "runtime.portal-hyprland",
            "runtime.lens-url-opener", "runtime.fx-loader-tools", "install.privileged-file-tools",
        })
        self.assertEqual(sorted(ids - set(PACMAN_CAPABILITY_PROVIDERS)), [])
