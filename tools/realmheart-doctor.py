#!/usr/bin/env python3
from __future__ import annotations

import os
import sys
from pathlib import Path

prefix = Path(__file__).resolve().parents[1]
python_root = prefix / "share" / "realmheart" / "python"
manifest_root = prefix / "share" / "realmheart" / "components"
if python_root.is_dir():
    sys.path.insert(0, str(python_root))
    os.environ.setdefault("REALMHEART_DOCTOR_MANIFEST_DIR", str(manifest_root))
else:
    # Repository development invocation.
    sys.path.insert(0, str(prefix))
    os.environ.setdefault("REALMHEART_DOCTOR_MANIFEST_DIR", str(prefix / "components"))

from realmheart_doctor.cli import main

raise SystemExit(main())
