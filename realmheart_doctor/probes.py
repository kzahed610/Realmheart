"""Compatibility-facing health probe exports.

The implementation is in :mod:`realmheart_doctor.health`; this module keeps the
probe-oriented import spelling discoverable without creating a second executor.
"""
from .health import *  # noqa: F401,F403
from .health import __all__ as _HEALTH_ALL
from .health import HealthCheckExecutor, HealthCheckReport, HealthCheckResult

ProbeExecutor = HealthCheckExecutor
ProbeReport = HealthCheckReport
ProbeResult = HealthCheckResult

__all__ = [
    *_HEALTH_ALL,
    "ProbeExecutor",
    "ProbeReport",
    "ProbeResult",
]
