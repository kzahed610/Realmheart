"""Installer-wide constants kept deliberately small and explicit."""

from __future__ import annotations

# Realmheart ships the installer as part of the Realmheart release.  Keep the
# persisted installer provenance aligned with the product release; the version
# contract test guards this against future CMake/installer drift.
INSTALLER_VERSION = "0.7.8"
STATE_SCHEMA_VERSION = 1
JOURNAL_SCHEMA_VERSION = 1
INSTALLED_STATE_SCHEMA_VERSION = 2
TRANSACTION_PREFIX = "RH"

INSTALLER_STATE_DIRNAME = "realmheart-installer"
REALMHEART_STATE_DIRNAME = "realmheart"
LOCK_FILENAME = "realmheart-installer.lock"
RECOVERY_RESERVE_FILENAME = ".recovery-reserve"
RECOVERY_RESERVE_BYTES = 16 * 1024
