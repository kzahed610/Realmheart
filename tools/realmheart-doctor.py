#!/usr/bin/env python3
from __future__ import annotations

import os
import sys
from pathlib import Path

prefix = Path(__file__).resolve().parents[1]
python_root = prefix / "share" / "realmheart" / "python"
manifest_root = prefix / "share" / "realmheart" / "components"
if python_root.is_dir():
    # Installed invocation.  The canonical manifest intentionally refuses to
    # expand privileged installation tokens unless their roots are explicit
    # absolute values.  Recreate the layout used by Realmheart's installer so
    # normal installed Doctor runs can resolve $PREFIX/$LIBEXEC/$SYSCONF
    # without requiring callers (including the boot service) to seed them.
    # Explicit caller overrides remain authoritative for diagnostic fixtures or
    # deliberately non-standard installations.
    os.environ.setdefault("PREFIX", str(prefix))
    os.environ.setdefault("LIBEXEC", str(prefix / "libexec"))
    os.environ.setdefault("SYSCONF", "/etc")
    sys.path.insert(0, str(python_root))
    os.environ.setdefault("REALMHEART_DOCTOR_MANIFEST_DIR", str(manifest_root))
else:
    # Repository development invocation.
    sys.path.insert(0, str(prefix))
    os.environ.setdefault("REALMHEART_DOCTOR_MANIFEST_DIR", str(prefix / "components"))

from realmheart_doctor.cli import main

raise SystemExit(main())
