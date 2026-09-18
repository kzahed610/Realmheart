# Realmheart Doctor incident

## What broke

    screenshot

## Current state

    failed

## Observed failure

    canonical artifact is absent

## Doctor diagnosis

    COMPONENT_ARTIFACT_MISSING

## Confidence

    HIGH

## Expected dependency state

    component passes its canonical health checks

## Detected dependency state

    [
      {
        "check_id": "check.screenshot.binary.exists",
        "reason_code": "artifact_missing",
        "status": "fail"
      }
    ]

## Last-known-good dependency state

    {
      "captured_at": "2026-09-17T21:00:00+00:00",
      "manifest_digest": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "release_version": "0.7.8"
    }

## Changes since last healthy state

    [
      {
        "current": "4.22.4-1",
        "package": "gtk4",
        "previous": "4.20.1-1",
        "type": "PACKAGE_TRANSACTION"
      }
    ]

## Repair attempts

    [
      {
        "action_type": "REBUILD_COMPONENT",
        "detail": "build failed with status 1",
        "fingerprint": "0123456789abcdef",
        "risk": "CONFIRM",
        "status": "failed",
        "timestamp": "2026-09-18T10:05:00+00:00"
      }
    ]

## Final conclusion

    unresolved

## Relevant sanitized logs

    [
      {
        "details": {
          "check_id": "check.screenshot.binary.exists"
        },
        "event_type": "HEALTH_CHECK_FAILED",
        "summary": "component screenshot was diagnosed as failed",
        "timestamp": "2026-09-18T10:00:00+00:00"
      },
      {
        "details": {
          "statuses": [
            "failed"
          ]
        },
        "event_type": "REPAIR_ATTEMPTED",
        "summary": "1 repair step(s) recorded; outcome failed",
        "timestamp": "2026-09-18T10:05:00+00:00"
      }
    ]

## Incident ID

    RH-20260918-001
