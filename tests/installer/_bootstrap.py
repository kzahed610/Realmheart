from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALLER_ROOT = REPO_ROOT / "installer"
if str(INSTALLER_ROOT) not in sys.path:
    sys.path.insert(0, str(INSTALLER_ROOT))
