"""Hyprland version parsing and release compatibility policy."""

from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum


_VERSION_RE = re.compile(r"(?<!\d)(\d+)\.(\d+)(?:\.(\d+))?")

HYPRLAND_MINIMUM = (0, 56, 1)
HYPRLAND_PREFERRED = (0, 56, 2)
HYPRLAND_TESTED_MINOR_LINES = frozenset({(0, 56), (0, 57)})


class HyprlandCompatibility(str, Enum):
    PREFERRED = "preferred"
    SUPPORTED = "supported"
    UNKNOWN = "unknown"
    INCOMPATIBLE = "incompatible"
    UNAVAILABLE = "unavailable"
    UNPARSEABLE = "unparseable"


@dataclass(frozen=True, order=True)
class ParsedVersion:
    major: int
    minor: int
    patch: int = 0

    @property
    def tuple(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"


def parse_version(text: str | None) -> ParsedVersion | None:
    if not text:
        return None
    match = _VERSION_RE.search(text)
    if not match:
        return None
    return ParsedVersion(
        int(match.group(1)),
        int(match.group(2)),
        int(match.group(3) or 0),
    )


def classify_hyprland(version: ParsedVersion | None, *, available: bool = True) -> HyprlandCompatibility:
    if not available:
        return HyprlandCompatibility.UNAVAILABLE
    if version is None:
        return HyprlandCompatibility.UNPARSEABLE
    if version.tuple < HYPRLAND_MINIMUM:
        return HyprlandCompatibility.INCOMPATIBLE
    if version.tuple == HYPRLAND_PREFERRED:
        return HyprlandCompatibility.PREFERRED
    if (version.major, version.minor) in HYPRLAND_TESTED_MINOR_LINES:
        return HyprlandCompatibility.SUPPORTED
    # Realmheart FX is ABI-sensitive. A newer version is not automatically
    # blessed merely because it satisfies a numeric lower bound.
    return HyprlandCompatibility.UNKNOWN
