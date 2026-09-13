"""Small no-shell command runner used by observational preflight probes."""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


@dataclass(frozen=True)
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str = ""
    stderr: str = ""
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out


class CommandRunner:
    """Execute bounded read-only probes without ever invoking a shell."""

    def __init__(self, *, env: Mapping[str, str] | None = None) -> None:
        self.env = dict(os.environ if env is None else env)

    def which(self, executable: str) -> str | None:
        return shutil.which(executable, path=self.env.get("PATH"))

    def run(
        self,
        argv: Sequence[str],
        *,
        timeout: float | None = 5.0,
        env: Mapping[str, str] | None = None,
        cwd: Path | None = None,
        interactive: bool = False,
    ) -> CommandResult:
        command = tuple(str(item) for item in argv)
        merged_env = dict(self.env)
        if env:
            merged_env.update(env)
        try:
            completed = subprocess.run(
                command,
                cwd=cwd,
                env=merged_env,
                stdin=None if interactive else subprocess.DEVNULL,
                stdout=None if interactive else subprocess.PIPE,
                stderr=None if interactive else subprocess.PIPE,
                text=True,
                shell=False,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            stdout = exc.stdout if isinstance(exc.stdout, str) else ""
            stderr = exc.stderr if isinstance(exc.stderr, str) else ""
            return CommandResult(command, 124, stdout=stdout, stderr=stderr, timed_out=True)
        except OSError as exc:
            return CommandResult(command, 127, stderr=str(exc))
        return CommandResult(
            command,
            completed.returncode,
            stdout=completed.stdout or "",
            stderr=completed.stderr or "",
        )
