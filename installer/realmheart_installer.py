#!/usr/bin/env python3
"""User-facing Realmheart installer entry point."""

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from realmheart_installer.cli import main


if __name__ == "__main__":
    raise SystemExit(main())
