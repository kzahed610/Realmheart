"""Realmheart installer safety kernel.

This package intentionally starts with transaction/recovery primitives before any
live Realmheart deployment behavior is wired in.
"""

from .constants import INSTALLER_VERSION

__all__ = ["INSTALLER_VERSION"]
