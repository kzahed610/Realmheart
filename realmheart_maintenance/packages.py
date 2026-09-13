"""Tool-independent package provenance record shape.

Package *names* remain adapter-specific; this module only supplies shared record
vocabulary for receipts/diagnostics.
"""
from dataclasses import dataclass

@dataclass(frozen=True)
class PackageProvenanceRecord:
    dependency_id: str
    package: str
    installed_before: bool
    version_before: str | None
    installed_by_transaction: bool
    version_after: str | None
    install_result: str
