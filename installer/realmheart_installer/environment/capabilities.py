"""Realmheart pre-manifest dependency capability probes.

This table is intentionally capability-centric. Package names belong to package
manager adapters later; Phase 5 only establishes what is objectively present.
"""

from __future__ import annotations

import os
import re
import tempfile
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Iterable, Mapping

from realmheart_maintenance.manifest import CapabilitySpec as ManifestCapabilitySpec, ManifestRegistry, load_manifest

from .command import CommandRunner
from .support import parse_version


class CapabilityState(str, Enum):
    PASS = "pass"
    MISSING = "missing"
    FAILED = "failed"
    NOT_APPLICABLE = "not_applicable"


class RequirementLevel(str, Enum):
    REQUIRED = "required"
    COMPONENT = "component"
    SOFT = "soft"


class DependencyLifecycle(str, Enum):
    BUILD = "build"
    INSTALL = "install"
    RUNTIME = "runtime"
    VERIFICATION = "verification"


@dataclass(frozen=True)
class CapabilityResult:
    capability_id: str
    display_name: str
    state: CapabilityState
    requirement: RequirementLevel
    lifecycle: tuple[DependencyLifecycle, ...]
    detail: str
    version: str | None = None
    executable: str | None = None
    component: str | None = None

    @property
    def satisfied(self) -> bool:
        return self.state in {CapabilityState.PASS, CapabilityState.NOT_APPLICABLE}


@dataclass(frozen=True)
class ExecutableSpec:
    capability_id: str
    display_name: str
    executable: str
    requirement: RequirementLevel
    lifecycle: tuple[DependencyLifecycle, ...]
    component: str | None = None
    version_argv: tuple[str, ...] | None = None
    minimum_version: tuple[int, int, int] | None = None


@dataclass(frozen=True)
class PkgConfigSpec:
    capability_id: str
    display_name: str
    module: str
    requirement: RequirementLevel
    lifecycle: tuple[DependencyLifecycle, ...]
    component: str | None = None
    minimum_version: str | None = None


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _default_registry() -> ManifestRegistry:
    return load_manifest(_repo_root() / "components")


def _requirement(value: str) -> RequirementLevel:
    return RequirementLevel(value)


def _lifecycle(values: tuple[str, ...]) -> tuple[DependencyLifecycle, ...]:
    return tuple(DependencyLifecycle(value) for value in values)


def _executable_spec_from_manifest(spec: ManifestCapabilitySpec) -> ExecutableSpec:
    args = spec.probe.args
    minimum = args.get("minimum_version")
    parsed_minimum: tuple[int, int, int] | None = None
    if isinstance(minimum, str):
        parsed = parse_version(minimum)
        if parsed is not None:
            parsed_minimum = parsed.tuple
    return ExecutableSpec(
        spec.id,
        spec.display_name,
        str(args["executable"]),
        _requirement(spec.requirement),
        _lifecycle(spec.lifecycle),
        component=spec.component_id,
        version_argv=tuple(str(item) for item in args.get("version_argv", ())) or None,
        minimum_version=parsed_minimum,
    )


def _pkg_config_spec_from_manifest(spec: ManifestCapabilitySpec) -> PkgConfigSpec:
    args = spec.probe.args
    return PkgConfigSpec(
        spec.id,
        spec.display_name,
        str(args["module"]),
        _requirement(spec.requirement),
        _lifecycle(spec.lifecycle),
        component=spec.component_id,
        minimum_version=str(args["minimum_version"]) if args.get("minimum_version") else None,
    )


# Compatibility views for tests/third-party callers.  These are generated from
# the canonical TOML registry; they are no longer independent product truth.
try:
    _COMPAT_REGISTRY = _default_registry()
    EXECUTABLE_SPECS: tuple[ExecutableSpec, ...] = tuple(
        _executable_spec_from_manifest(spec)
        for spec in _COMPAT_REGISTRY.capabilities_in_order()
        if spec.probe.kind == "executable"
    )
    PKG_CONFIG_SPECS: tuple[PkgConfigSpec, ...] = tuple(
        _pkg_config_spec_from_manifest(spec)
        for spec in _COMPAT_REGISTRY.capabilities_in_order()
        if spec.probe.kind == "pkg_config"
    )
except Exception:
    # Manifest errors are surfaced explicitly by preflight.  Avoid turning an
    # import into an opaque traceback before the installer can render the real
    # source-invalid diagnosis.
    EXECUTABLE_SPECS = ()
    PKG_CONFIG_SPECS = ()


class CapabilityScanner:
    def __init__(
        self,
        runner: CommandRunner,
        *,
        temp_root: Path,
        env: Mapping[str, str] | None = None,
        registry: ManifestRegistry | None = None,
    ) -> None:
        self.runner = runner
        self.temp_root = temp_root
        self.env = dict(os.environ if env is None else env)
        self.registry = registry

    def scan_all(self) -> tuple[CapabilityResult, ...]:
        registry = self.registry or _default_registry()
        return tuple(self.probe_manifest_capability(spec) for spec in registry.capabilities_in_order())

    def probe_manifest_capability(self, spec: ManifestCapabilitySpec) -> CapabilityResult:
        kind = spec.probe.kind
        args = spec.probe.args
        if kind == "executable":
            result = self.probe_executable(_executable_spec_from_manifest(spec))
        elif kind == "pkg_config":
            result = self.probe_pkg_config(_pkg_config_spec_from_manifest(spec))
        elif kind == "cxx26":
            result = self.probe_cxx26()
        elif kind == "opencv_cmake":
            result = self.probe_opencv_ximgproc()
        elif kind == "pam_link":
            result = self.probe_pam_devel()
        elif kind == "cmake_gtest":
            result = self.probe_cmake_gtest()
        elif kind == "tesseract_language":
            result = self.probe_tesseract_english()
        elif kind == "networkmanager_backend":
            result = self.probe_networkmanager_backend()
        elif kind == "bluetooth_backend":
            result = self.probe_bluetooth_backend()
        elif kind == "power_profiles_backend":
            result = self.probe_power_profiles_backend()
        elif kind == "systemd_user":
            result = self.probe_systemd_user()
        elif kind == "portal_unit":
            result = self.probe_portal_unit()
        elif kind == "any_command":
            result = self.probe_any_command(
                spec.id, spec.display_name, tuple(str(item) for item in args["commands"]),
                requirement=_requirement(spec.requirement), lifecycle=_lifecycle(spec.lifecycle), component=spec.component_id,
            )
        elif kind == "command_group":
            result = self.probe_command_group(
                spec.id, spec.display_name, tuple(str(item) for item in args["commands"]),
                requirement=_requirement(spec.requirement), lifecycle=_lifecycle(spec.lifecycle), component=spec.component_id,
            )
        else:
            raise ValueError(f"unsupported canonical probe kind: {kind}")
        return CapabilityResult(
            capability_id=spec.id,
            display_name=spec.display_name,
            state=result.state,
            requirement=_requirement(spec.requirement),
            lifecycle=_lifecycle(spec.lifecycle),
            detail=result.detail,
            version=result.version,
            executable=result.executable,
            component=spec.component_id,
        )

    def probe_executable(self, spec: ExecutableSpec) -> CapabilityResult:
        path = self.runner.which(spec.executable)
        if not path:
            return self._missing(spec.capability_id, spec.display_name, spec.requirement, spec.lifecycle, spec.component, f"{spec.executable} not found in PATH")
        version_text: str | None = None
        if spec.version_argv:
            result = self.runner.run((path, *spec.version_argv), timeout=4.0)
            combined = (result.stdout or result.stderr).strip()
            version_text = combined.splitlines()[0] if combined else None
            if spec.minimum_version:
                parsed = parse_version(combined)
                if parsed is None or parsed.tuple < spec.minimum_version:
                    wanted = ".".join(str(part) for part in spec.minimum_version)
                    return CapabilityResult(
                        spec.capability_id,
                        spec.display_name,
                        CapabilityState.FAILED,
                        spec.requirement,
                        spec.lifecycle,
                        f"found {version_text or 'unknown version'}, requires >= {wanted}",
                        version=version_text,
                        executable=path,
                        component=spec.component,
                    )
        return CapabilityResult(
            spec.capability_id,
            spec.display_name,
            CapabilityState.PASS,
            spec.requirement,
            spec.lifecycle,
            "available",
            version=version_text,
            executable=path,
            component=spec.component,
        )

    def probe_pkg_config(self, spec: PkgConfigSpec) -> CapabilityResult:
        pkg_config = self.runner.which("pkg-config")
        if not pkg_config:
            return self._missing(spec.capability_id, spec.display_name, spec.requirement, spec.lifecycle, spec.component, "pkg-config unavailable")
        if spec.minimum_version:
            argv = (pkg_config, f"--atleast-version={spec.minimum_version}", spec.module)
        else:
            argv = (pkg_config, "--exists", spec.module)
        check = self.runner.run(argv, timeout=4.0)
        if not check.ok:
            suffix = f" >= {spec.minimum_version}" if spec.minimum_version else ""
            return self._missing(spec.capability_id, spec.display_name, spec.requirement, spec.lifecycle, spec.component, f"pkg-config module {spec.module}{suffix} not satisfied")
        version = self.runner.run((pkg_config, "--modversion", spec.module), timeout=4.0)
        return CapabilityResult(
            spec.capability_id,
            spec.display_name,
            CapabilityState.PASS,
            spec.requirement,
            spec.lifecycle,
            "pkg-config probe passed",
            version=version.stdout.strip() if version.ok and version.stdout.strip() else None,
            executable=pkg_config,
            component=spec.component,
        )

    def probe_wayland_scanner(self) -> CapabilityResult:
        spec = ExecutableSpec("wayland.scanner", "wayland-scanner", "wayland-scanner", RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), component="screenshot")
        return self.probe_executable(spec)

    def probe_cxx26(self) -> CapabilityResult:
        compiler = self.runner.which("c++") or self.runner.which("g++") or self.runner.which("clang++")
        if not compiler:
            return self._missing("build.cxx26", "C++26 compiler", RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "realmheart-fx", "no C++ compiler found")
        self.temp_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with tempfile.TemporaryDirectory(prefix="cxx26-", dir=self.temp_root) as tmp:
            source = Path(tmp) / "probe.cpp"
            source.write_text("consteval int f(){return 26;} int main(){static_assert(f()==26);}\n", encoding="utf-8")
            result = self.runner.run((compiler, "-std=c++26", "-fsyntax-only", str(source)), timeout=10.0)
        if not result.ok:
            return CapabilityResult("build.cxx26", "C++26 compiler", CapabilityState.FAILED, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "compiler rejected -std=c++26", executable=compiler, component="realmheart-fx")
        return CapabilityResult("build.cxx26", "C++26 compiler", CapabilityState.PASS, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "compiler accepts -std=c++26", executable=compiler, component="realmheart-fx")

    def probe_opencv_ximgproc(self) -> CapabilityResult:
        cmake = self.runner.which("cmake")
        if not cmake:
            return self._missing("opencv.ximgproc", "OpenCV core/imgproc/ximgproc", RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "screenshot", "cmake unavailable for OpenCV component probe")
        self.temp_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with tempfile.TemporaryDirectory(prefix="opencv-", dir=self.temp_root) as tmp:
            root = Path(tmp)
            source = root / "src"
            build = root / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(realmheart_opencv_probe LANGUAGES CXX)\n"
                "find_package(OpenCV REQUIRED COMPONENTS core imgproc ximgproc)\n"
                "message(STATUS \"REALMHEART_OPENCV_VERSION=${OpenCV_VERSION}\")\n",
                encoding="utf-8",
            )
            result = self.runner.run((cmake, "-S", str(source), "-B", str(build)), timeout=15.0)
        if not result.ok:
            detail = (result.stderr or result.stdout).strip().splitlines()
            tail = detail[-1] if detail else "CMake could not resolve required OpenCV components"
            return CapabilityResult("opencv.ximgproc", "OpenCV core/imgproc/ximgproc", CapabilityState.FAILED, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), tail, executable=cmake, component="screenshot")
        combined = result.stdout + "\n" + result.stderr
        match = re.search(r"REALMHEART_OPENCV_VERSION=([^\s]+)", combined)
        return CapabilityResult("opencv.ximgproc", "OpenCV core/imgproc/ximgproc", CapabilityState.PASS, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "CMake resolved OpenCV core, imgproc and ximgproc", version=match.group(1) if match else None, executable=cmake, component="screenshot")

    def probe_pam_devel(self) -> CapabilityResult:
        compiler = self.runner.which("c++") or self.runner.which("g++") or self.runner.which("clang++")
        if not compiler:
            return self._missing("pam.devel", "PAM development interface", RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "lockscreen-auth", "no C++ compiler available for PAM probe")
        self.temp_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with tempfile.TemporaryDirectory(prefix="pam-", dir=self.temp_root) as tmp:
            root = Path(tmp)
            source = root / "probe.cpp"
            binary = root / "probe"
            source.write_text(
                "#include <security/pam_appl.h>\n"
                "int main(){ pam_handle_t* p=nullptr; (void)p; return PAM_SUCCESS == 0 ? 0 : 1; }\n",
                encoding="utf-8",
            )
            result = self.runner.run((compiler, str(source), "-lpam", "-o", str(binary)), timeout=10.0)
        if not result.ok:
            return CapabilityResult("pam.devel", "PAM development interface", CapabilityState.FAILED, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "PAM headers and/or libpam link interface unavailable", executable=compiler, component="lockscreen-auth")
        return CapabilityResult("pam.devel", "PAM development interface", CapabilityState.PASS, RequirementLevel.REQUIRED, (DependencyLifecycle.BUILD,), "PAM header/link probe passed", executable=compiler, component="lockscreen-auth")

    def probe_cmake_gtest(self) -> CapabilityResult:
        cmake = self.runner.which("cmake")
        if not cmake:
            return self._missing("verification.gtest", "GoogleTest CMake package", RequirementLevel.SOFT, (DependencyLifecycle.VERIFICATION,), "native-tests", "cmake unavailable for GTest probe")
        self.temp_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with tempfile.TemporaryDirectory(prefix="gtest-", dir=self.temp_root) as tmp:
            root = Path(tmp)
            source = root / "src"
            build = root / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(realmheart_gtest_probe LANGUAGES CXX)\n"
                "find_package(GTest REQUIRED)\n",
                encoding="utf-8",
            )
            result = self.runner.run((cmake, "-S", str(source), "-B", str(build)), timeout=15.0)
        if not result.ok:
            return self._missing("verification.gtest", "GoogleTest CMake package", RequirementLevel.SOFT, (DependencyLifecycle.VERIFICATION,), "native-tests", "GTest CMake package not found")
        return CapabilityResult("verification.gtest", "GoogleTest CMake package", CapabilityState.PASS, RequirementLevel.SOFT, (DependencyLifecycle.VERIFICATION,), "CMake resolved GTest", executable=cmake, component="native-tests")

    def probe_tesseract_english(self) -> CapabilityResult:
        executable = self.runner.which("tesseract")
        if not executable:
            return self._missing("tesseract.lang.eng", "Tesseract English language data", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "screenshot-ocr", "tesseract executable unavailable")
        result = self.runner.run((executable, "--list-langs"), timeout=6.0)
        if not result.ok:
            return CapabilityResult("tesseract.lang.eng", "Tesseract English language data", CapabilityState.FAILED, RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "unable to enumerate Tesseract languages", executable=executable, component="screenshot-ocr")
        languages = {line.strip() for line in result.stdout.splitlines() if line.strip() and not line.lower().startswith("list of available languages")}
        if "eng" not in languages:
            return self._missing("tesseract.lang.eng", "Tesseract English language data", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "screenshot-ocr", "eng language data not installed")
        return CapabilityResult("tesseract.lang.eng", "Tesseract English language data", CapabilityState.PASS, RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "eng language data available", executable=executable, component="screenshot-ocr")

    def probe_any_command(
        self,
        capability_id: str,
        display_name: str,
        commands: tuple[str, ...],
        *,
        requirement: RequirementLevel,
        lifecycle: tuple[DependencyLifecycle, ...],
        component: str | None = None,
    ) -> CapabilityResult:
        for command in commands:
            path = self.runner.which(command)
            if path:
                return CapabilityResult(capability_id, display_name, CapabilityState.PASS, requirement, lifecycle, f"using {command}", executable=path, component=component)
        return self._missing(capability_id, display_name, requirement, lifecycle, component, "none available: " + ", ".join(commands))

    def probe_command_group(
        self,
        capability_id: str,
        display_name: str,
        commands: tuple[str, ...],
        *,
        requirement: RequirementLevel,
        lifecycle: tuple[DependencyLifecycle, ...],
        component: str | None = None,
    ) -> CapabilityResult:
        missing = [command for command in commands if not self.runner.which(command)]
        if missing:
            return self._missing(capability_id, display_name, requirement, lifecycle, component, "missing commands: " + ", ".join(missing))
        return CapabilityResult(capability_id, display_name, CapabilityState.PASS, requirement, lifecycle, "all required commands available", component=component)

    def probe_networkmanager_backend(self) -> CapabilityResult:
        executable = self.runner.which("nmcli")
        if not executable:
            return self._missing(
                "runtime.nmcli",
                "NetworkManager Wi-Fi control",
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                "wifi",
                "nmcli not found in PATH",
            )
        result = self.runner.run((executable, "-t", "-f", "STATE", "general"), timeout=4.0)
        if not result.ok:
            detail = (result.stderr or result.stdout or "NetworkManager backend is not reachable").strip()
            return CapabilityResult(
                "runtime.nmcli",
                "NetworkManager Wi-Fi control",
                CapabilityState.FAILED,
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                detail,
                executable=executable,
                component="wifi",
            )
        state = result.stdout.strip() or "reachable"
        return CapabilityResult(
            "runtime.nmcli",
            "NetworkManager Wi-Fi control",
            CapabilityState.PASS,
            RequirementLevel.COMPONENT,
            (DependencyLifecycle.RUNTIME,),
            f"NetworkManager backend reachable ({state})",
            executable=executable,
            component="wifi",
        )

    def probe_bluetooth_backend(self) -> CapabilityResult:
        executable = self.runner.which("bluetoothctl")
        if not executable:
            return self._missing(
                "runtime.bluetoothctl",
                "BlueZ Bluetooth control",
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                "bluetooth",
                "bluetoothctl not found in PATH",
            )
        result = self.runner.run((executable, "list"), timeout=4.0)
        if not result.ok:
            detail = (result.stderr or result.stdout or "BlueZ backend is not reachable").strip()
            return CapabilityResult(
                "runtime.bluetoothctl",
                "BlueZ Bluetooth control",
                CapabilityState.FAILED,
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                detail,
                executable=executable,
                component="bluetooth",
            )
        controllers = tuple(line.strip() for line in result.stdout.splitlines() if line.strip())
        if not controllers:
            return CapabilityResult(
                "runtime.bluetoothctl",
                "BlueZ Bluetooth control",
                CapabilityState.NOT_APPLICABLE,
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                "BlueZ is reachable but no Bluetooth controller is present",
                executable=executable,
                component="bluetooth",
            )
        return CapabilityResult(
            "runtime.bluetoothctl",
            "BlueZ Bluetooth control",
            CapabilityState.PASS,
            RequirementLevel.COMPONENT,
            (DependencyLifecycle.RUNTIME,),
            f"BlueZ reachable with {len(controllers)} controller(s)",
            executable=executable,
            component="bluetooth",
        )

    def probe_power_profiles_backend(self) -> CapabilityResult:
        executable = self.runner.which("powerprofilesctl")
        if not executable:
            return self._missing(
                "runtime.powerprofilesctl",
                "Power profile control",
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                "power-profiles",
                "powerprofilesctl not found in PATH",
            )
        result = self.runner.run((executable, "list"), timeout=5.0)
        if not result.ok:
            detail = (result.stderr or result.stdout or "power-profiles-daemon backend is not reachable").strip()
            return CapabilityResult(
                "runtime.powerprofilesctl",
                "Power profile control",
                CapabilityState.FAILED,
                RequirementLevel.COMPONENT,
                (DependencyLifecycle.RUNTIME,),
                detail,
                executable=executable,
                component="power-profiles",
            )
        profiles = tuple(line.strip() for line in result.stdout.splitlines() if line.strip())
        return CapabilityResult(
            "runtime.powerprofilesctl",
            "Power profile control",
            CapabilityState.PASS,
            RequirementLevel.COMPONENT,
            (DependencyLifecycle.RUNTIME,),
            f"power-profiles-daemon backend reachable ({len(profiles)} output line(s))",
            executable=executable,
            component="power-profiles",
        )

    def probe_systemd_user(self) -> CapabilityResult:
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            return self._missing("runtime.systemd-user", "systemd user manager", RequirementLevel.REQUIRED, (DependencyLifecycle.RUNTIME,), None, "systemctl unavailable")
        result = self.runner.run((systemctl, "--user", "show-environment"), timeout=4.0)
        if not result.ok:
            return CapabilityResult("runtime.systemd-user", "systemd user manager", CapabilityState.FAILED, RequirementLevel.REQUIRED, (DependencyLifecycle.RUNTIME,), "systemctl --user is not usable in this session", executable=systemctl)
        return CapabilityResult("runtime.systemd-user", "systemd user manager", CapabilityState.PASS, RequirementLevel.REQUIRED, (DependencyLifecycle.RUNTIME,), "user manager reachable", executable=systemctl)

    def probe_portal_unit(self) -> CapabilityResult:
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            return self._missing("runtime.portal-hyprland", "Hyprland XDG portal", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "desktop-portal", "systemctl unavailable")
        result = self.runner.run((systemctl, "--user", "list-unit-files", "xdg-desktop-portal-hyprland.service", "--no-legend", "--no-pager"), timeout=4.0)
        if not result.ok or "xdg-desktop-portal-hyprland.service" not in result.stdout:
            return self._missing("runtime.portal-hyprland", "Hyprland XDG portal", RequirementLevel.COMPONENT, (DependencyLifecycle.RUNTIME,), "desktop-portal", "user service unit not found")
        return CapabilityResult(
            "runtime.portal-hyprland",
            "Hyprland XDG portal",
            CapabilityState.PASS,
            RequirementLevel.COMPONENT,
            (DependencyLifecycle.RUNTIME,),
            "service unit installed (structural preflight only; runtime health is verified later)",
            executable=systemctl,
            component="desktop-portal",
        )

    @staticmethod
    def _missing(
        capability_id: str,
        display_name: str,
        requirement: RequirementLevel,
        lifecycle: tuple[DependencyLifecycle, ...],
        component: str | None,
        detail: str,
    ) -> CapabilityResult:
        return CapabilityResult(capability_id, display_name, CapabilityState.MISSING, requirement, lifecycle, detail, component=component)


def required_failures(results: Iterable[CapabilityResult]) -> tuple[CapabilityResult, ...]:
    return tuple(result for result in results if result.requirement is RequirementLevel.REQUIRED and not result.satisfied)
