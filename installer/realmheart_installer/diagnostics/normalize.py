"""Privacy-preserving normalization for Phase-15 reports."""
from __future__ import annotations

import re
from pathlib import Path

from ..context import XdgPaths

_CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")
_OTHER_HOME_RE = re.compile(r"(?<![A-Za-z0-9_.-])/(?:home|Users)/[^/\s]+")
_TMP_RE = re.compile(r"(?<![A-Za-z0-9_.-])/(?:tmp|var/tmp)/[^\s'\"<>]+")


class PathNormalizer:
    def __init__(self, *, paths: XdgPaths, source_root: Path) -> None:
        replacements = {
            str(source_root.resolve(strict=False)): "$SOURCE_ROOT",
            str(paths.config_home.resolve(strict=False)): "$XDG_CONFIG_HOME",
            str(paths.state_home.resolve(strict=False)): "$XDG_STATE_HOME",
            str(paths.data_home.resolve(strict=False)): "$XDG_DATA_HOME",
            str(paths.cache_home.resolve(strict=False)): "$XDG_CACHE_HOME",
            str(paths.runtime_dir.resolve(strict=False)): "$XDG_RUNTIME_DIR",
            str(paths.home.resolve(strict=False)): "$HOME",
        }
        self._replacements = tuple(sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True))

    def text(self, value: object | None, *, limit: int = 640) -> str | None:
        if value is None:
            return None
        text = _CONTROL_RE.sub("?", str(value)).replace("\r", " ").replace("\n", " ")
        for raw, token in self._replacements:
            if raw:
                text = text.replace(raw, token)
        text = _OTHER_HOME_RE.sub("$HOME", text)
        text = _TMP_RE.sub("$TMP", text)
        text = " ".join(text.split())
        return text[:limit]

    def path(self, value: object | None) -> str | None:
        return self.text(value, limit=1024)
