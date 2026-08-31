#!/usr/bin/env python3
"""Derive and verify Realmheart's explicit Tessia display-tier packages."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import shutil
import sys
from pathlib import Path
from typing import Any

from PIL import Image, ImageChops


TIERS: dict[str, float] = {
    "1080p": 1.0,
    "1440p": 4.0 / 3.0,
    "4k": 2.0,
}
VISIBLE_FILTER = "LANCZOS"
MASK_FILTER = "NEAREST"
FLOW_FILTER = "BILINEAR"
FLOAT_DIGITS = 6


def round_half_away_from_zero(value: float) -> int:
    if value >= 0.0:
        return math.floor(value + 0.5)
    return math.ceil(value - 0.5)


def scaled_dimension(value: int, scale: float) -> int:
    return round_half_away_from_zero(value * scale)


def scaled_float(value: float, scale: float) -> float:
    rounded = round(value * scale, FLOAT_DIGITS)
    if rounded == 0.0:
        return 0.0
    return rounded


def filter_for_asset(filename: str) -> tuple[int, str]:
    lowered = filename.lower()
    if "mask" in lowered:
        return Image.Resampling.NEAREST, MASK_FILTER
    if "flow" in lowered:
        return Image.Resampling.BILINEAR, FLOW_FILTER
    return Image.Resampling.LANCZOS, VISIBLE_FILTER


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def scaled_manifest(source: dict[str, Any], tier: str, scale: float) -> dict[str, Any]:
    manifest = copy.deepcopy(source)
    manifest.pop("scaleVariant", None)
    manifest.pop("derivedFrom", None)
    manifest["displayTier"] = tier

    if tier == "1080p":
        manifest["derivation"] = {
            "sourceTier": "canonical-1080p",
            "scale": "1",
            "resampler": "byte-preserving-copy",
        }
    else:
        manifest["derivedFrom"] = "1080p"
        manifest["derivation"] = {
            "sourceTier": "1080p",
            "scale": "4/3" if tier == "1440p" else "2",
            "resampler": {
                "visible": VISIBLE_FILTER,
                "mask": MASK_FILTER,
                "flow": FLOW_FILTER,
            },
            "rounding": "round-half-away-from-zero",
        }

    canvas = manifest["sourceCanvas"]
    canvas["width"] = scaled_dimension(int(canvas["width"]), scale)
    canvas["height"] = scaled_dimension(int(canvas["height"]), scale)

    for asset in manifest["assets"].values():
        asset["offset"]["x"] = scaled_float(float(asset["offset"]["x"]), scale)
        asset["offset"]["y"] = scaled_float(float(asset["offset"]["y"]), scale)
        asset["size"]["width"] = scaled_dimension(int(asset["size"]["width"]), scale)
        asset["size"]["height"] = scaled_dimension(int(asset["size"]["height"]), scale)

    for family in manifest.get("families", {}).values():
        crop = family["crop"]
        crop["x"] = scaled_float(float(crop["x"]), scale)
        crop["y"] = scaled_float(float(crop["y"]), scale)
        crop["width"] = scaled_dimension(int(crop["width"]), scale)
        crop["height"] = scaled_dimension(int(crop["height"]), scale)
        padding = family["padding"]
        for edge in ("left", "top", "right", "bottom"):
            padding[edge] = scaled_dimension(int(padding[edge]), scale)

    return manifest


def expected_asset_size(source: Image.Image, scale: float) -> tuple[int, int]:
    return (
        scaled_dimension(source.width, scale),
        scaled_dimension(source.height, scale),
    )


def resize_asset(source_path: Path, output_path: Path, scale: float) -> None:
    resampling, _ = filter_for_asset(source_path.name)
    with Image.open(source_path) as source:
        source_rgba = source.convert("RGBA")
        target_size = expected_asset_size(source_rgba, scale)
        resized = source_rgba.resize(target_size, resample=resampling)
        resized.save(output_path, format="PNG", optimize=False, compress_level=9)


def source_manifest(root: Path) -> dict[str, Any]:
    return load_json(root / "manifest.json")


def expected_assets(manifest: dict[str, Any]) -> list[str]:
    return [str(asset["file"]) for asset in manifest["assets"].values()]


def generate(root: Path) -> None:
    canonical_dir = root / "1x"
    source = source_manifest(canonical_dir)
    asset_files = expected_assets(source)

    for tier, scale in TIERS.items():
        output_dir = root / tier
        output_dir.mkdir(parents=True, exist_ok=True)
        output_manifest = scaled_manifest(source, tier, scale)
        write_json(output_dir / "manifest.json", output_manifest)

        for filename in asset_files:
            source_path = canonical_dir / filename
            output_path = output_dir / filename
            if not source_path.is_file():
                raise FileNotFoundError(source_path)
            if tier == "1080p":
                shutil.copyfile(source_path, output_path)
            else:
                resize_asset(source_path, output_path, scale)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_manifest_geometry(
    manifest: dict[str, Any],
    source: dict[str, Any],
    tier: str,
    scale: float,
) -> list[str]:
    errors: list[str] = []
    if manifest.get("displayTier") != tier:
        errors.append(f"{tier}: displayTier metadata mismatch")
    if tier == "1080p":
        if "derivedFrom" in manifest:
            errors.append("1080p: canonical package must not have derivedFrom")
    elif manifest.get("derivedFrom") != "1080p":
        errors.append(f"{tier}: derivedFrom must be 1080p")

    expected = scaled_manifest(source, tier, scale)
    if manifest.get("sourceCanvas") != expected.get("sourceCanvas"):
        errors.append(f"{tier}: sourceCanvas mismatch")
    if manifest.get("assets") != expected.get("assets"):
        errors.append(f"{tier}: asset geometry mismatch")
    if manifest.get("families") != expected.get("families"):
        errors.append(f"{tier}: family geometry mismatch")
    return errors


def verify(root: Path) -> list[str]:
    canonical_dir = root / "1x"
    source = source_manifest(canonical_dir)
    errors: list[str] = []
    asset_files = expected_assets(source)

    for tier, scale in TIERS.items():
        output_dir = root / tier
        manifest_path = output_dir / "manifest.json"
        if not manifest_path.is_file():
            errors.append(f"{tier}: missing manifest.json")
            continue
        manifest = load_json(manifest_path)
        errors.extend(verify_manifest_geometry(manifest, source, tier, scale))

        for filename in asset_files:
            source_path = canonical_dir / filename
            output_path = output_dir / filename
            if not output_path.is_file():
                errors.append(f"{tier}: missing asset {filename}")
                continue
            if tier == "1080p":
                if sha256(source_path) != sha256(output_path):
                    errors.append(f"1080p: asset is not byte-preserving: {filename}")
                continue

            resampling, filter_name = filter_for_asset(filename)
            with Image.open(source_path) as source_image, Image.open(output_path) as output_image:
                source_rgba = source_image.convert("RGBA")
                expected_size = expected_asset_size(source_rgba, scale)
                if output_image.mode != "RGBA":
                    errors.append(f"{tier}: {filename} is not RGBA")
                if output_image.size != expected_size:
                    errors.append(
                        f"{tier}: {filename} size {output_image.size} != {expected_size}"
                    )
                expected_image = source_rgba.resize(expected_size, resample=resampling)
                if ImageChops.difference(expected_image, output_image.convert("RGBA")).getbbox() is not None:
                    errors.append(f"{tier}: {filename} differs from direct {filter_name} transform")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="Tessia asset root")
    parser.add_argument("command", choices=("generate", "verify"))
    args = parser.parse_args()

    try:
        if args.command == "generate":
            generate(args.root)
            errors = verify(args.root)
        else:
            errors = verify(args.root)
    except (OSError, ValueError, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print(f"Tessia tier {args.command} passed: {', '.join(TIERS)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
