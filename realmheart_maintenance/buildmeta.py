"""Stable build-provenance vocabulary shared by installer and future Doctor."""
from dataclasses import dataclass

@dataclass(frozen=True)
class BuildProvenanceHint:
    dependency_id: str
    capture_version: bool = True
    capture_abi: bool = False
    capture_source_revision: bool = False
