#!/usr/bin/env python3
"""Build a dev prefix tree so the Realmheart Doctor can resolve its manifest
artifact paths when invoked from the repository (no `cmake --install` needed).

The manifest declares artifacts under `$PREFIX/bin`, `$PREFIX/lib/realmheart`,
`$PREFIX/share/realmheart`, and `$LIBEXEC/realmheart`. A repo build drops its
binaries directly in `build-<name>/` and keeps assets/styles/effects in the repo
root — neither matches the manifest layout, so the dev invocation must expose a
prefix tree that does.

The Doctor deliberately rejects symlinks (`symlink_forbidden`), so this script
uses **hardlinks** for files (zero disk cost, same filesystem, and not symlinks)
and real copies for directories. It is idempotent and only writes inside the
output prefix (`.devprefix/` by default).

Usage:
    python3 tools/build-dev-prefix.py [--build-dir build-hybrid]
"""
from __future__ import annotations

import argparse
import os
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = "build-hybrid"


def _link_file(src: Path, dst: Path, *, executable: bool = False,
               force_copy: bool = False) -> None:
    """Place a real file at dst (hardlink preferred, copy fallback).

    When ``executable`` is set, dst is guaranteed to carry the executable bit
    (the manifest declares some artifacts as ``mode = "0755"`` regardless of the
    source checkout's mode).  When ``force_copy`` is set, dst is always a fresh
    copy so a later chmod on dst can never mutate the source (hardlinks share an
    inode, so chmod'ing dst would also flip the source's mode).
    """
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink() or dst.exists():
        dst.unlink()
    if not force_copy:
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    else:
        shutil.copy2(src, dst)
    if executable:
        dst.chmod(dst.stat().st_mode | 0o111)


def _place_dir(src: Path, dst: Path) -> None:
    """Place a real directory at dst (copy, no symlinks)."""
    if dst.is_symlink():
        dst.unlink()
    elif dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--prefix", default=None,
                        help="output prefix (default: <repo>/.devprefix)")
    args = parser.parse_args()

    build_dir = (REPO / args.build_dir).resolve()
    prefix = Path(args.prefix).resolve() if args.prefix else REPO / ".devprefix"

    if not build_dir.is_dir():
        print(f"error: build dir not found: {build_dir}")
        return 1

    prefix.mkdir(parents=True, exist_ok=True)
    bindir = prefix / "bin"
    libexec = prefix / "libexec" / "realmheart"
    librealmheart = prefix / "lib" / "realmheart"
    share = prefix / "share" / "realmheart"
    for d in (bindir, libexec, librealmheart, share):
        d.mkdir(parents=True, exist_ok=True)

    # --- bin/ : map each manifest bin artifact to its build output ------------
    bin_map = {
        "realmheart": "realmheart",
        "realmheart-power-menu-renderer": "realmheart-power-menu-renderer",
        "realmheart-eventd": "realmheart-eventd",
        "realmheart-event": "realmheart-event",
        "realmheart-screenshot": "realmheart-screenshot",
        "realmheart-screenshot-regions": "realmheart-screenshot-regions",
        "realmheart-wallpaper-renderer": "realmheart-wallpaper-renderer",
        "realmheart-doctor": REPO / "tools" / "realmheart-doctor.py",
    }
    for name, target in bin_map.items():
        target_path = Path(target)
        if not target_path.is_absolute():
            target_path = build_dir / target_path
        if target_path.exists():
            _link_file(target_path.resolve(), bindir / name)
        else:
            print(f"warn: missing build artifact, skipping bin/{name}: {target_path}")

    # --- libexec/realmheart/ : auth helper + update marker --------------------
    auth_helper = build_dir / "realmheart-auth-helper"
    if auth_helper.exists():
        _link_file(auth_helper.resolve(), libexec / "realmheart-auth-helper")
    marker = REPO / "config" / "pacman" / "realmheart-post-update-marker.sh"
    if marker.exists():
        # force_copy: the manifest declares mode 0755, but the source checkout is
        # 0644; a hardlink would make the chmod mutate the repo file.
        _link_file(marker.resolve(), libexec / "realmheart-post-update-marker",
                   executable=True, force_copy=True)

    # --- lib/realmheart/ : realmheart-fx.so ---------------------------------
    fx = build_dir / "realmheart-fx.so"
    if fx.exists():
        _link_file(fx.resolve(), librealmheart / "realmheart-fx.so")
    else:
        print("warn: realmheart-fx.so not built, skipping lib/realmheart/realmheart-fx.so")

    # --- share/realmheart/ : assets, styles, effects, build-fingerprints, ----
    #     python runtime, components -----------------------------------------
    for name in ("assets", "styles", "effects"):
        src = REPO / name
        if src.is_dir():
            _place_dir(src.resolve(), share / name)

    bf = build_dir / "build-fingerprints"
    if bf.is_dir():
        _place_dir(bf.resolve(), share / "build-fingerprints")

    pyroot = share / "python"
    pyroot.mkdir(parents=True, exist_ok=True)
    for pkg in ("realmheart_doctor", "realmheart_maintenance"):
        src = REPO / pkg
        if src.is_dir():
            _place_dir(src.resolve(), pyroot / pkg)

    comp = share / "components"
    if (REPO / "components").is_dir():
        _place_dir((REPO / "components").resolve(), comp)

    # --- report -------------------------------------------------------------
    print(f"dev prefix ready: {prefix}")
    print(f"  PREFIX={prefix}")
    print(f"  LIBEXEC={prefix / 'libexec'}")
    print(f"  SYSCONF=/etc  (system paths resolve to real host locations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())