#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from realmheart_maintenance.manifest import ManifestError, load_manifest
from realmheart_maintenance.repository import validate_repository


def main() -> int:
    try:
        registry = load_manifest(ROOT / "components")
        validation = validate_repository(ROOT, registry)
    except ManifestError as exc:
        print(f"Realmheart canonical manifest: FAIL\n  - {exc}", file=sys.stderr)
        return 1
    if not validation.ok:
        print("Realmheart canonical manifest: FAIL", file=sys.stderr)
        for error in validation.errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("Realmheart canonical manifest: PASS")
    print(f"Schema: {registry.schema_version}")
    print(f"Release: {registry.release_version}")
    print(f"Digest: {registry.digest}")
    print(
        "Graph: "
        f"{len(registry.components)} components, "
        f"{len(registry.dependencies)} dependencies, "
        f"{len(registry.capabilities)} capabilities, "
        f"{len(registry.artifacts)} artifacts, "
        f"{len(registry.build_units)} build units"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
