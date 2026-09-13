"""Compatibility import surface for shared manifest consumers."""
from .manifest import ManifestRegistry, ManifestError, load_manifest
__all__ = ["ManifestRegistry", "ManifestError", "load_manifest"]
