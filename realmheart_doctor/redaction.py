"""Conservative filtering of report text; never a proof of secret absence.

Identity inputs are explicit for deterministic tests. Defaults inspect only the
local account identity, never the environment or unrelated user data.
"""
from __future__ import annotations

import ipaddress
import os
from pathlib import Path
import pwd
import re
import socket

REDACTED = "[REDACTED]"
_IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")
_IPV6 = re.compile(r"(?<![\w:])[0-9a-fA-F:.]*:[0-9a-fA-F:.]+(?:%[\w.-]+)?(?![\w:])")
_AUTH = re.compile(r"(?im)\b(?:authorization|proxy-authorization)\s*:[^\r\n]*")
_ASSIGNMENT = re.compile(
    r'''(?ix)\b(?:api[_-]?key|access[_-]?token|refresh[_-]?token|token|password|passwd|secret|ssid)\b["']?\s*[:=]\s*(?:"[^"\n]*"|'[^'\n]*'|[^\s,;}"']+)'''
)
_TOKEN = re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9_]+|github_pat_[A-Za-z0-9_]+)\b")
_BEARER = re.compile(r"(?i)\bBearer\s+[^\s,;]+")
_HOME = re.compile(r"/home/[^/\s]+")
_PRIVATE_KEY = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(?:-----END [A-Z ]*PRIVATE KEY-----|\Z)", re.S)
_CONNECTION = re.compile(r"\b[a-zA-Z][a-zA-Z0-9+.-]*://[^\s/@]+:[^\s/@]+@[^\s]+")


def scan_secrets(text: str) -> tuple[str, ...]:
    """Return finding categories only; never copy potential secrets to logs."""
    patterns = (("private_key", _PRIVATE_KEY), ("connection_string", _CONNECTION),
                ("provider_token", _TOKEN), ("bearer_token", _BEARER),
                ("secret_assignment", _ASSIGNMENT))
    return tuple(name for name, pattern in patterns
                 if any(REDACTED not in match.group() for match in pattern.finditer(text)))



def sanitize_text(text: str, *, username: str | None = None,
                  hostname: str | None = None, home: str | None = None) -> str:
    """Remove recognized private data without deleting diagnostic context."""
    if username is None:
        username = pwd.getpwuid(os.getuid()).pw_name
    if hostname is None:
        hostname = socket.gethostname()
    if home is None:
        home = str(Path.home())
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = _PRIVATE_KEY.sub(REDACTED, text)
    text = _CONNECTION.sub(REDACTED, text)
    text = _AUTH.sub(lambda m: m.group().split(":", 1)[0] + ": " + REDACTED, text)
    text = _ASSIGNMENT.sub(lambda m: re.split(r"[:=]", m.group(), maxsplit=1)[0].rstrip(' \"\'') + "=" + REDACTED, text)
    text = _TOKEN.sub(REDACTED, text)
    text = _BEARER.sub("Bearer " + REDACTED, text)
    if home and home != "/":
        text = re.sub(re.escape(home) + r"(?=/|\s|$)", "~", text)
    text = _HOME.sub("~", text)
    for identity in (username, hostname):
        if identity:
            text = re.sub(r"(?<![\w-])" + re.escape(identity) + r"(?![\w-])", REDACTED, text)

    def address(match: re.Match[str]) -> str:
        try:
            ipaddress.ip_address(match.group().split("%", 1)[0])
        except ValueError:
            return match.group()
        return REDACTED

    return _IPV4.sub(address, _IPV6.sub(address, text))
