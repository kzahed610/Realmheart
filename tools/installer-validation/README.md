# Realmheart Installer — Phase 19 real-system validation

This directory documents the destructive/manual half of the Phase-19 validation
matrix. The executable harness is `tools/validate-realmheart-installer.py`.

## Safety rule

**Never run the live matrix first on a workstation you care about.** Use a VM,
filesystem/system snapshot, or a disposable account on a disposable system. Some
scenarios intentionally interrupt a live transaction, replace Hyprland config,
or exercise privileged Realmheart artifacts under `/usr/local` and `/etc/pam.d`.
A disposable *user account alone* is not sufficient isolation for scenarios that
touch those system-wide paths unless the whole host is disposable/snapshotted.

## Recommended campaign

1. Create a clean supported Hyprland VM snapshot.
2. Keep `phase19-report.json` and the evidence directory outside the snapshot so
   reset does not erase results.
3. Run `fixture-matrix` once on the exact candidate source tree.
4. Run `host-audit` after each materially different VM/source environment.
5. Use `guide <scenario>` before changing the snapshot.
6. Run one destructive scenario, collect evidence, and `record` its outcome.
7. Reset to the appropriate snapshot before the next scenario.
8. Finish with `summary`; only 13/13 live PASS plus fixture/host PASS is acceptance.

## Scenario ordering suggestion

A convenient order that minimizes snapshot preparation is:

```text
clean-user
  -> custom-hypr
  -> custom-kitty
  -> custom-fish
  -> non-default-xdg
  -> conflicting-owned-files
  -> missing-soft-dependency
  -> multi-monitor
  -> broken-component
  -> controlled-interrupt

managed older-version snapshot
  -> upgrade

same-version managed snapshot
  -> reinstall

managed newer-version snapshot
  -> downgrade
```

The version-transition rows need genuine managed receipts/baselines from the
corresponding versions. Do not synthesize a receipt by hand merely to make the
matrix green; that would test JSON editing rather than installation history.

## Evidence

Useful evidence is concise: terminal log, final receipt/report, checksum output
for preserved files, and recovery output for interruption cases. Avoid copying
private application data merely because the ledger supports arbitrary evidence
files. The harness hashes evidence and can place a private mode-0600 copy under
the chosen evidence directory.
