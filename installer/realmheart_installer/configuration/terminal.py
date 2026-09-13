"""Terminal-specific rendering and validation helpers.

The public Realmheart Terminal bundle is treated as a behavioral/content source,
not as the mutation boundary.  Everything here is deterministic and consumes
resolved paths from the approved InstallationPlan.
"""

from __future__ import annotations

import hashlib
import os
import stat
import tomllib
from pathlib import Path

from ..environment.command import CommandRunner
from ..errors import ConfigurationIntegrationError
from ..filesystem.compare import fingerprint_path
from ..planning.models import ConfigAction
from .models import GeneratedArtifactResult, VerificationResult

KITTY_BEGIN = "# BEGIN Realmheart Terminal Theme (managed)"
KITTY_END = "# END Realmheart Terminal Theme (managed)"


def render_action_content(action: ConfigAction, *, source_root: Path) -> tuple[bytes, int]:
    """Render one source-backed Phase-11 config action exactly as planned."""

    if action.source is None:
        rendered = _render_source_less_action(action)
        if rendered is not None:
            return rendered
        raise ConfigurationIntegrationError(
            f"Config action {action.id} has no source payload",
            code="RH_CONFIG_SOURCE_MISSING",
            details={"action_id": action.id, "target": action.target},
        )
    source = Path(action.source)
    _validate_source(source, Path(source_root))
    raw = source.read_bytes()
    mode = int(action.mode, 8) if action.mode is not None else stat.S_IMODE(source.stat(follow_symlinks=False).st_mode)

    if action.render_strategy is None:
        return raw, mode

    text = raw.decode("utf-8")
    values = dict(action.render_values)
    if action.render_strategy == "terminal-kitty-dropin-v1":
        state_theme = values.get("STATE_THEME")
        if not state_theme:
            raise ConfigurationIntegrationError(
                "Terminal Kitty renderer is missing STATE_THEME",
                code="RH_CONFIG_RENDER_CONTRACT_INVALID",
                details={"action_id": action.id},
            )
        rendered = (
            "# Realmheart terminal theme include.\n"
            "# The generator refreshes this state file from the active Matugen palette.\n"
            f"include {state_theme}\n"
        )
        return rendered.encode("utf-8"), mode

    if action.render_strategy in {"rewrite-default-xdg-v1", "token-substitution-v1", "realmheart-fx-loader-v1"}:
        rendered = text
        for before, after in action.render_values:
            rendered = rendered.replace(before, after)
        if action.render_strategy in {"token-substitution-v1", "realmheart-fx-loader-v1"} and "@" in rendered:
            # Only source templates using @TOKEN@ reach this renderer. Refuse a
            # partially substituted unit rather than installing latent garbage.
            import re
            unresolved = sorted(set(re.findall(r"@[A-Z0-9_]+@", rendered)))
            if unresolved:
                raise ConfigurationIntegrationError(
                    f"Unresolved config template tokens: {', '.join(unresolved)}",
                    code="RH_CONFIG_TEMPLATE_UNRESOLVED",
                    details={"action_id": action.id, "tokens": unresolved},
                )
        return rendered.encode("utf-8"), mode

    raise ConfigurationIntegrationError(
        f"Unsupported Phase-11 render strategy: {action.render_strategy}",
        code="RH_CONFIG_RENDER_STRATEGY_UNSUPPORTED",
        details={"action_id": action.id, "strategy": action.render_strategy},
    )



def _render_source_less_action(action: ConfigAction) -> tuple[bytes, int] | None:
    """Render installer-owned source-less user artifacts from the approved plan.

    The strategy name and every machine-specific value are frozen into the
    InstallationPlan.  Live execution therefore does not rediscover executable
    paths or silently make a new policy decision after consent.
    """

    values = dict(action.render_values)
    if action.render_strategy == "realmheart-core-user-service-v1":
        binary = values.get("REALMHEART_BINARY")
        args = values.get("ARGS", "")
        if not binary:
            _invalid_source_less(action, "REALMHEART_BINARY")
        text = (
            "[Unit]\n"
            "Description=Realmheart Shell\n"
            "After=graphical-session.target\n"
            "PartOf=graphical-session.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            f"ExecStart={binary} {args}\n"
            "Restart=on-failure\n"
            "RestartSec=1\n\n"
            "[Install]\n"
            "WantedBy=graphical-session.target\n"
        )
        return text.encode("utf-8"), 0o644

    if action.render_strategy == "realmheart-eventd-user-service-v1":
        binary = values.get("REALMHEART_EVENTD_BINARY")
        if not binary:
            _invalid_source_less(action, "REALMHEART_EVENTD_BINARY")
        text = (
            "[Unit]\n"
            "Description=Realmheart Event Surface daemon\n"
            "After=graphical-session.target\n"
            "PartOf=graphical-session.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            f"ExecStart={binary}\n"
            "Restart=on-failure\n"
            "RestartSec=1\n\n"
            "[Install]\n"
            "WantedBy=graphical-session.target\n"
        )
        return text.encode("utf-8"), 0o644

    if action.render_strategy == "realmheart-lock-session-v1":
        binary = values.get("REALMHEART_BINARY")
        systemctl = values.get("SYSTEMCTL")
        loginctl = values.get("LOGINCTL")
        if not binary or not systemctl or not loginctl:
            _invalid_source_less(action, "REALMHEART_BINARY/SYSTEMCTL/LOGINCTL")
        text = (
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            f"REALMHEART={_shell_single_quote(binary)}\n"
            f"SYSTEMCTL={_shell_single_quote(systemctl)}\n"
            f"LOGINCTL={_shell_single_quote(loginctl)}\n"
            'exec "$REALMHEART" --lock-session --systemctl "$SYSTEMCTL" --loginctl "$LOGINCTL"\n'
        )
        return text.encode("utf-8"), 0o755

    return None


def _invalid_source_less(action: ConfigAction, field: str) -> None:
    raise ConfigurationIntegrationError(
        f"Config action {action.id} is missing rendered value {field}",
        code="RH_CONFIG_RENDER_CONTRACT_INVALID",
        details={"action_id": action.id, "field": field},
    )


def _shell_single_quote(value: str) -> str:
    # Values come from the approved plan, but quote them defensively because the
    # generated wrapper is a shell script.  No eval is used.
    return "'" + value.replace("'", "'\"'\"'") + "'"

def kitty_managed_body(action: ConfigAction) -> str:
    if action.render_strategy != "kitty-managed-include-v1":
        raise ConfigurationIntegrationError(
            "Kitty managed-block action is missing the canonical renderer contract",
            code="RH_CONFIG_RENDER_CONTRACT_INVALID",
            details={"action_id": action.id},
        )
    include_path = dict(action.render_values).get("INCLUDE_PATH")
    if not include_path:
        raise ConfigurationIntegrationError(
            "Kitty managed-block action has no INCLUDE_PATH",
            code="RH_CONFIG_RENDER_CONTRACT_INVALID",
            details={"action_id": action.id},
        )
    return f"include {include_path}"


def generated_results(artifact_paths: dict[str, Path]) -> tuple[GeneratedArtifactResult, ...]:
    results: list[GeneratedArtifactResult] = []
    for artifact_id, path in sorted(artifact_paths.items()):
        exists = path.is_file() and not path.is_symlink()
        size = path.stat().st_size if exists else None
        results.append(
            GeneratedArtifactResult(
                artifact_id=artifact_id,
                path=str(path),
                exists=exists,
                size=size,
                fingerprint=fingerprint_path(path),
            )
        )
    return tuple(results)


def validate_terminal_state(
    *,
    home: Path,
    config_home: Path,
    state_home: Path,
    generated_paths: dict[str, Path],
    runner: CommandRunner,
    pycache_root: Path,
) -> tuple[VerificationResult, ...]:
    """Validate the installed/generated terminal contract without a shell."""

    checks: list[VerificationResult] = []
    generator = config_home / "realmheart/scripts/terminal/generate-theme.py"
    kitty_dropin = config_home / "kitty/realmheart-theme.conf"
    kitty_conf = config_home / "kitty/kitty.conf"
    fish_theme = config_home / "fish/conf.d/realmheart-theme.fish"
    fish_starship = config_home / "fish/conf.d/realmheart-starship.fish"
    service = config_home / "systemd/user/realmheart-terminal-theme.service"
    path_unit = config_home / "systemd/user/realmheart-terminal-theme.path"

    python = runner.which("python3") or runner.which("python")
    if python:
        pycache_root.mkdir(parents=True, exist_ok=True)
        result = runner.run(
            [python, "-m", "py_compile", str(generator)],
            timeout=10.0,
            env={"PYTHONPYCACHEPREFIX": str(pycache_root)},
        )
        checks.append(VerificationResult("terminal.generator.py_compile", result.ok, _command_detail(result)))
    else:
        checks.append(VerificationResult("terminal.generator.py_compile", False, "Python interpreter not found"))

    starship_path = generated_paths.get("terminal.generated-starship")
    if starship_path and starship_path.is_file() and not starship_path.is_symlink():
        try:
            text = starship_path.read_text(encoding="utf-8")
            tomllib.loads(text)
            unresolved = "@@" in text
            checks.append(VerificationResult(
                "terminal.generated-starship.toml",
                not unresolved,
                "valid TOML with no unresolved placeholders" if not unresolved else "unresolved @@ placeholder remains",
            ))
        except (OSError, UnicodeError, tomllib.TOMLDecodeError) as exc:
            checks.append(VerificationResult("terminal.generated-starship.toml", False, str(exc)))
    else:
        checks.append(VerificationResult("terminal.generated-starship.toml", False, "generated Starship config missing"))

    fish = runner.which("fish")
    for check_id, path in (
        ("terminal.fish-theme.syntax", fish_theme),
        ("terminal.fish-starship.syntax", fish_starship),
    ):
        if fish:
            result = runner.run([fish, "-n", str(path)], timeout=8.0)
            checks.append(VerificationResult(check_id, result.ok, _command_detail(result)))
        else:
            checks.append(VerificationResult(check_id, False, "Fish executable not found"))

    starship = runner.which("starship")
    if starship and starship_path:
        result = runner.run(
            [starship, "prompt"],
            timeout=8.0,
            env={"STARSHIP_CONFIG": str(starship_path), "HOME": str(home), "XDG_CONFIG_HOME": str(config_home), "XDG_STATE_HOME": str(state_home)},
        )
        checks.append(VerificationResult("terminal.starship.render", result.ok, _command_detail(result)))
    else:
        checks.append(VerificationResult("terminal.starship.render", False, "Starship executable or generated config missing"))

    kitty_theme = generated_paths.get("terminal.generated-kitty")
    kitty_ok = bool(kitty_theme and kitty_theme.is_file() and not kitty_theme.is_symlink() and kitty_theme.stat().st_size > 0)
    checks.append(VerificationResult("terminal.generated-kitty.nonempty", kitty_ok, str(kitty_theme) if kitty_theme else "missing"))

    block_ok, block_detail = _verify_kitty_block(kitty_conf, kitty_dropin)
    checks.append(VerificationResult("terminal.kitty.managed-block", block_ok, block_detail))

    expected_theme = state_home / "realmheart/theme/kitty-theme.conf"
    try:
        dropin_text = kitty_dropin.read_text(encoding="utf-8")
        dropin_ok = f"include {expected_theme}" in dropin_text and "~/.local/state/realmheart/theme/kitty-theme.conf" not in dropin_text
        # The default state root is allowed to be represented by its resolved
        # absolute path too. The second guard only rejects stale literal source
        # when a non-default XDG state root is in use.
        if state_home == config_home.parent / ".local/state":
            dropin_ok = f"include {expected_theme}" in dropin_text
        checks.append(VerificationResult(
            "terminal.kitty.dropin-target",
            dropin_ok,
            f"expected include {expected_theme}",
        ))
    except OSError as exc:
        checks.append(VerificationResult("terminal.kitty.dropin-target", False, str(exc)))

    for check_id, unit_path in (
        ("terminal.systemd.service-structure", service),
        ("terminal.systemd.path-structure", path_unit),
    ):
        ok, detail = _validate_unit_text(unit_path, config_home=config_home, state_home=state_home)
        checks.append(VerificationResult(check_id, ok, detail))

    return tuple(checks)


def _verify_kitty_block(kitty_conf: Path, kitty_dropin: Path) -> tuple[bool, str]:
    try:
        text = kitty_conf.read_text(encoding="utf-8")
    except OSError as exc:
        return False, str(exc)
    begin = text.count(KITTY_BEGIN)
    end = text.count(KITTY_END)
    include = text.count(f"include {kitty_dropin}")
    ok = begin == 1 and end == 1 and include == 1
    return ok, f"begin={begin} end={end} include={include}"


def _validate_unit_text(path: Path, *, config_home: Path, state_home: Path) -> tuple[bool, str]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        return False, str(exc)
    if "[Unit]" not in text:
        return False, "missing [Unit] section"
    if path.suffix == ".service":
        if "[Service]" not in text or "ExecStart=" not in text:
            return False, "missing [Service]/ExecStart"
        expected = str(config_home / "realmheart/scripts/terminal/generate-theme.py")
        default = "%h/.config/realmheart/scripts/terminal/generate-theme.py"
        if expected not in text and default not in text:
            return False, "generator ExecStart does not resolve to Realmheart generator"
        if "date +%s%N" in text and "date +%%s%%N" not in text:
            return False, "systemd percent escaping for Fish epoch is invalid"
    else:
        if "[Path]" not in text or "PathModified=" not in text or "PathChanged=" not in text:
            return False, "missing [Path] watcher directives"
        expected = str(state_home / "realmheart/theme-palette.tsv")
        default = "%h/.local/state/realmheart/theme-palette.tsv"
        if expected not in text and default not in text:
            return False, "watcher path does not resolve to target palette"
    return True, "unit structure/path contract valid"


def _validate_source(source: Path, source_root: Path) -> None:
    root = Path(os.path.abspath(source_root))
    candidate = Path(os.path.abspath(source))
    try:
        common = Path(os.path.commonpath([root, candidate]))
    except ValueError:
        common = Path("/")
    if common != root or not source.is_file() or source.is_symlink():
        raise ConfigurationIntegrationError(
            f"Unsafe/missing configuration source: {source}",
            code="RH_CONFIG_SOURCE_INVALID",
            details={"source": str(source), "source_root": str(source_root)},
        )


def _command_detail(result) -> str:
    if result.ok:
        return "PASS"
    text = (result.stderr or result.stdout or f"exit {result.returncode}").strip().replace("\n", " ")
    return text[:240]
