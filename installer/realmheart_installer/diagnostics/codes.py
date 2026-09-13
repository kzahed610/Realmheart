"""Stable Phase-15 normalized diagnostic error vocabulary."""
from __future__ import annotations

import re

_TOKEN_RE = re.compile(r"[^A-Z0-9]+")


def verification_error_code(check_id: str) -> str:
    token = _TOKEN_RE.sub("_", check_id.upper()).strip("_")
    if token.startswith("VERIFY_"):
        token = token[len("VERIFY_"):]
    elif token.startswith("CHECK_"):
        token = token[len("CHECK_"):]
    return f"RH_VERIFY_{token}_FAILED"


def stage_code(stage: str, state: str) -> str:
    left = _TOKEN_RE.sub("_", stage.upper()).strip("_")
    right = _TOKEN_RE.sub("_", state.upper()).strip("_")
    return f"RH_{left}_{right}"
