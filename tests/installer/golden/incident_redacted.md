# Realmheart Doctor incident

## What broke

    lockscreen-auth

## Current state

    failed

## Observed failure

    helper failed for uid 1000 at ~/.local/bin/realmheart-auth-helper; network peer [REDACTED]; token=[REDACTED]

## Doctor diagnosis

    OBSERVED_FAILURE

## Confidence

    MEDIUM

## Final conclusion

    unresolved

## Relevant sanitized logs

    [
      {
        "details": {
          "check_id": "check.auth.helper.exists"
        },
        "event_type": "HEALTH_CHECK_FAILED",
        "summary": "component lockscreen-auth was diagnosed as failed",
        "timestamp": "2026-09-18T12:00:00+00:00"
      }
    ]

## Incident ID

    RH-20260918-002
