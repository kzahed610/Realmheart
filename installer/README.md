# Realmheart Installer

Realmheart's installer is a normal-user, transactional deployment orchestrator for
the current Realmheart release. It performs environment/dependency preflight,
canonical source/manifest validation, install-mode detection, isolated CMake
build + `DESTDIR` staging, permanent/pre-adoption/previous-version safety backup,
journaled live commit, ownership-aware Hyprland/Kitty/Fish integration, privileged
auth/PAM verification, observed-state verification, final keep/rollback handling,
installed-state receipts, diagnostics, uninstall, and crash recovery.

## Public quick start

Run from any working directory; the entry point resolves the Realmheart checkout
from its own location rather than trusting the caller's current directory.

```bash
# Inspect the complete authoritative plan. No Realmheart transaction is created.
python3 /path/to/Realmheart/installer/realmheart_installer.py --dry-run install

# From a reviewed READY plan, perform the install. On CachyOS/Arch the verified
# pacman adapter may install missing package-provided dependencies after consent.
python3 /path/to/Realmheart/installer/realmheart_installer.py --install-dependencies install
```

Never start the installer with `sudo`. It runs as the desktop user and requests
narrow elevation only for package/system operations that require it.

Useful operational commands:

```bash
python3 installer/realmheart_installer.py preflight
python3 installer/realmheart_installer.py dependencies
python3 installer/realmheart_installer.py verify-current
python3 installer/realmheart_installer.py diagnose-current
python3 installer/realmheart_installer.py recovery-list
python3 installer/realmheart_installer.py --dry-run uninstall
python3 installer/realmheart_installer.py --compare-config uninstall
python3 installer/realmheart_installer.py uninstall
```

The root `README.md` contains the canonical end-user installation path. The
sections below preserve the implementation milestones and deeper contracts for
maintainers; statements describing what was *not yet wired* at an earlier phase
are historical, not the current product state.

## Implementation history and technical contracts

Implemented Phase-5 preflight:

- `/etc/os-release`, architecture, kernel and package-manager detection;
- pacman as the only automatic-dependency-install-capable v1 adapter identity;
- non-Arch machines remain probeable instead of being rejected by distro name;
- Wayland/systemd-user/Hyprland session detection;
- Hyprland policy: `<0.56.1` incompatible, `0.56.2` preferred/battle-tested,
  `0.56.x` and `0.57.x` tested/supported, newer minor lines compatibility-unknown
  and requiring an explicit future-support decision;
- Hyprland monitor topology capture via `hyprctl monitors -j`;
- capability-centric build/runtime dependency scanning;
- C++26 compile probe for Realmheart FX;
- OpenCV `ximgproc` header/link-interface probe;
- Tesseract `eng` language-data probe;
- systemd user-manager and Hyprland portal-unit probes;
- disk/free-space visibility and target-root writability checks;
- structured JSON-serializable `EnvironmentSnapshot` and plain renderer;
- functional backend probes for NetworkManager and power-profiles-daemon, with
  Bluetooth correctly represented as not-applicable when no controller exists;
- structural-only wording for portal-unit availability;
- install-lifecycle classification for privileged file tools.

Implemented Phase-6 identity/mode detection:

- target Realmheart version parsed only from canonical `project(Realmheart VERSION ...)`;
- source Git commit/dirty provenance when a Git checkout is available;
- durable `$XDG_STATE_HOME/realmheart/installed-state.json` detection and schema validation;
- managed receipt precedence over legacy/development evidence;
- legacy `realmheart.service` adoption detection without executing user config;
- development/source-tree binary detection;
- trustworthy `realmheart --version` probing when the binary supports it;
- conservative source-checkout version inference for legacy services that point into
  the current Realmheart source tree;
- fresh/reinstall/upgrade/downgrade mode resolution;
- corrupt/unsupported receipts and unprovable unmanaged versions block mutation;
- legacy/development origin explicitly requires a future pre-adoption snapshot;
- native `realmheart --version` now derives from CMake `PROJECT_VERSION` instead of
  stale hand-maintained CLI text.

Capability identity, lifecycle, severity, component ownership, probe metadata,
artifacts, health-check metadata and BuildUnit relationships now come from the
shared `components/*.toml` schema. Pacman package spelling stays adapter-specific.
The same direct capability probes are rerun after package installation.

## Run tests

```sh
PYTHONPATH=installer python3 -m unittest discover -s tests/installer -t . -v
```

## Run preflight

```sh
python3 installer/realmheart_installer.py preflight
python3 installer/realmheart_installer.py --json preflight
```

A default invocation currently performs preflight and then stops before any live
Realmheart deployment. `preflight` returns non-zero when the machine is not ready.

## Phase 7: pacman dependency adapter

`dependencies` is the explicit package-facing command. Without
`--install-dependencies` it is read-only and prints the verified package plan.

```bash
python3 installer/realmheart_installer.py dependencies
python3 installer/realmheart_installer.py dependencies --install-dependencies
```

The adapter only uses configured pacman sync repositories. It never invokes an
AUR helper, never performs `pacman -Sy`, and never installs/replaces Hyprland or
the host init/session stack. Before mutation it records package state/version;
after pacman returns it queries package state again and the CLI re-runs the
direct Realmheart capability probes. Package-manager exit status is therefore
not treated as proof that a capability works.

New packages are recorded in transaction provenance. They are intentionally not
automatically removed during rollback; later uninstall/cleanup logic must use
that provenance and prefer leaving a harmless package over breaking the host.

## Phase 8: canonical shared product graph

`components/*.toml` is Realmheart-wide product metadata, not an installer-private
registry. `realmheart_maintenance` loads and validates the graph using stdlib
`tomllib`, computes a deterministic manifest-set SHA-256 digest, performs cycle
and reference validation, and can be imported by future Doctor without importing
installer handlers.

Validate manifest/repository drift with:

```bash
python3 tools/validate-realmheart-manifest.py
```

The validator cross-checks the CMake release/targets, declared source artifacts,
and Realmheart user-service references in shipped Hyprland config. Installer-only
custom behavior is bound separately by stable component ID.

## Phase 9: authoritative installation plan and truthful dry-run

Default invocation now produces one serialized `InstallationPlan` that combines
preflight, install mode, manifest identity, package actions, safety backups,
config ownership semantics, component/build-unit order, required FX policy,
privileged commits, user-service activation, health checks, and known disk needs.
The later live executor must consume this same plan rather than re-derive intent.

Dry-run is observational apart from the normal advisory runtime lock. It uses OS
temporary scratch for compile/link probes and does not create a transaction
directory, backup, `plan.json`, Realmheart config, or package mutation:

```bash
python3 installer/realmheart_installer.py --dry-run
python3 installer/realmheart_installer.py --dry-run --verbose
python3 installer/realmheart_installer.py --dry-run --json
```

Destructive/shared targets carry their observed fingerprints in the plan so a
future executor can revalidate compare-before-write preconditions immediately
before mutation. Legacy installations receive a pre-adoption snapshot; they are
never mislabeled as having a pristine pre-Realmheart baseline. Hyprland remains
a staged full-tree takeover with `custom/` preserved, while Kitty/Fish and the
Realmheart config namespace remain surgical/allowlisted. Source-less generated
user units/wrappers carry stable renderer IDs and resolved values in the plan.

## Phase 10: native build + validated DESTDIR staging

`build-stage` consumes the exact Phase-9 `InstallationPlan` and performs the
native Realmheart build entirely as the normal user. **Do not run the installer
with `sudo`.** The command may write installer-private transaction/cache state,
but it does not commit Realmheart payload into live `/usr/local`, `/etc`, user
configuration, or user services.

```bash
python3 installer/realmheart_installer.py --verbose build-stage
python3 installer/realmheart_installer.py --json build-stage
```

The Phase-10 executor:

- revalidates source version/revision/dirty state and canonical manifest digest;
- creates fresh installer-private build and `DESTDIR` stage directories;
- configures Ninja/Release with explicit GNU install directories and
  `REALMHEART_EVENTD_AUTOSTART=OFF`;
- exports `REALMHEART_EVENTD_AUTOSTART_DISABLE=1` while building;
- requires plugin, screenshot and native-wallpaper build paths enabled;
- builds every manifest-declared required native BuildUnit, including FX;
- probes the built `realmheart --version` and runs the selected side-effect-safe
  shell contract checks declared in the authoritative plan;
- runs unprivileged `cmake --install` with
  `REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1` and `DESTDIR`;
- validates required staged files/directories, executable bits, the staged
  setuid auth-helper mode, and required Realmheart FX payload;
- rejects canonical staged directory trees containing symlinks;
- records build/compiler/Hyprland/ABI/artifact provenance in `build-stage.json`;
- fingerprints every eventual live mutation target before and after the build
  and fails the stage if the build changed any of them.

The production build deliberately uses `BUILD_TESTING=OFF`: GTest and
`dbus-run-session` remain soft verification capabilities rather than accidental
hard production-build dependencies. Phase 10 runs the selected safe repository
contract tests directly instead.

A successful `build-stage` leaves the validated payload under the transaction's
installer-private `DESTDIR` for later phases. It does **not** mean Realmheart was
installed or activated on the live system.

## Phase 11: Realmheart configuration + terminal integration

Phase 11 binds the actual Realmheart Hyprland and Terminal v4.2 N1 ownership
contracts to the write-ahead transaction engine. The reusable
`ConfigurationIntegrator` is intentionally **not exposed as a live desktop CLI
command yet**; Phase 12 component execution will invoke it only after the durable
baseline/pre-adoption and build prerequisites are satisfied.

Implemented behavior:

- `config/hypr` is prepared as a same-filesystem sibling staging tree and swapped
  atomically while the current user's `hypr/custom/` subtree wins over release
  defaults and newly introduced defaults are seeded only when absent;
- Kitty remains shared user configuration: exactly one Realmheart managed include
  block is applied via the journaled managed-block primitive and unrelated bytes
  are preserved;
- Fish `config.fish` is a read-only invariant; Realmheart writes only its two
  `conf.d` drop-ins;
- the Kitty drop-in, Fish drop-ins, generator, and two user-systemd terminal units
  are installed as individually journaled Realmheart-owned files with canonical
  product modes from the shared manifest;
- non-default `XDG_CONFIG_HOME` / `XDG_STATE_HOME` are rendered into the Kitty
  include and trusted systemd templates rather than inheriting developer-machine
  paths;
- the terminal generator runs on the target environment and emits the four
  canonical generated artifacts under `$XDG_STATE_HOME/realmheart/theme/`;
- `theme-palette.tsv` is read-only broader Realmheart input and is never treated
  as terminal-owned state;
- generated state has its own WAL lifecycle and guarded path-specific rollback;
- terminal validation covers generator bytecode compilation, Starship TOML parsing,
  Fish syntax, Starship rendering, Kitty managed-block/drop-in correctness, generated
  Kitty state, and systemd unit/path structure;
- user-systemd watcher activation records prior enabled/active state, journals
  daemon-reload/enable/start/restart behavior, verifies the resulting path-unit
  state, and restores the previous state on rollback;
- synthetic watcher failure tests prove the filesystem transaction is rolled back
  instead of leaving a half-installed terminal integration.

The canonical manifest now declares all four generated terminal outputs and their
actual XDG **state** paths (`kitty-theme.conf`, `fish-theme.fish`, `starship.toml`,
`rail.png`). It also carries optional artifact mode metadata, so the generator is
installed as `0755` even if repository checkout mode bits differ.

## Phase 12: meaningful Realmheart component handlers

Phase 12 resolves the 18 canonical manifest components into real installer
execution units without introducing a second component database. Handler
footprints are derived from the approved `InstallationPlan`: native/system
artifact commits, configuration actions, user-service actions, BuildUnits,
health-check IDs, privileged targets, and rollback requirements all remain tied
to the exact plan the user inspected.

The component handler layer deliberately separates **orchestration** from the
mutation backend. A handler decides which transaction-aware operations belong to
one meaningful Realmheart subsystem and in what order; the backend performs those
operations through the live WAL/filesystem/service machinery. This keeps support
output meaningful (`Realmheart FX`, `Screenshot`, `Lockscreen Authentication`,
`Realmheart Terminal`, etc.) without permitting handlers to bypass the transaction
kernel with ad-hoc file operations.

Implemented handler semantics include:

- deterministic canonical topological execution with dynamic progress counts;
- local component failure containment and required-dependency `BLOCKED`
  propagation while unrelated branches continue;
- external dependency/package gating before a handler is allowed to execute;
- exact per-component footprints for artifacts, config actions, services,
  BuildUnits, health checks, privileged targets, and rollback classes;
- generic handlers for ordinary manifest-driven components plus explicit custom
  binding hooks for Hypr integration, Lockscreen Authentication, and Terminal;
- Terminal's Phase-11 watcher activation remains in the same configuration
  rollback domain, so the component executor cannot activate it twice;
- handler-local verification hooks for every component; Phase 13 expands these
  hooks into the full structural/security/health verification engine;
- reverse-order component rollback delegation using the handler's declared
  transaction operation IDs and rollback requirements;
- progress rendering that names actual Realmheart subsystems rather than
  anonymous numbered micro-operations.

Phase 12 still does **not** expose a top-level live deployment command. The
component mutation backend is the intentional boundary that the later live
transaction executor will satisfy after persistent backup/build prerequisites are
in force. This phase proves component ownership/choreography and failure
attribution without bypassing the safety sequence.

Inspect the resolved Phase-12 handler footprints without executing component
mutations:

```bash
python3 installer/realmheart_installer.py --dry-run component-plan
python3 installer/realmheart_installer.py --dry-run --json component-plan
```

The output uses the dynamic canonical component count and real display names.

## Phase 13: observed-state verification engine

Phase 13 separates desired-state planning from post-install truth. The verifier
consumes the approved `InstallationPlan` and canonical manifest, but component
health and future `installed-state.json` receipt inputs are assembled from the
machine state actually observed after deployment.

Verification is multi-layered:

- canonical structural health checks plus filesystem type/mode validation;
- fresh runtime/verification capability observations for capability-only as well
  as artifact-owning components;
- ownership-aware config invariants (Hypr staged takeover residue/structure,
  exactly one Kitty managed block, untouched personal `config.fish`, rendered
  terminal/clipboard files, generated terminal syntax/state checks);
- safe smoke probes only where the product exposes non-destructive contracts
  (`realmheart --version`, Event Surface CLI help/ping when its daemon is active);
- user-systemd enabled/active checks according to the approved service actions;
- Core-critical lockscreen auth-helper/PAM owner/mode/content checks without ever
  collecting or replaying authentication secrets;
- required FX live artifact, compatibility, and build/Hyprland ABI provenance
  checks;
- optional comparison of live immutable artifacts against the exact validated
  Phase-10 `DESTDIR` identities;
- install-health aggregation with internal dependency `BLOCKED` attribution;
- activation classification separate from install health. A verified install
  that still needs a fresh Hyprland session is recorded as
  `pending_session_restart` with runtime health `unknown`, never falsely promoted
  to runtime Last Known Good.

The receipt input assembly intentionally hashes/fingerprints only immutable
Realmheart release/system artifacts. Mutable/shared user configuration is
represented by minimal observed structure/ownership facts and targeted health
invariants rather than copied or hashed wholesale. The durable receipt itself is
not published by Phase 13; final keep/rollback decision logic owns that later
transactional commit.

A read-only current-machine inspection surface is available for diagnostics:

```bash
python3 installer/realmheart_installer.py verify-current
python3 installer/realmheart_installer.py --json verify-current
```

`verify-current` never creates an install transaction or mutates Realmheart. On a
legacy/pre-commit machine it may correctly report failures because it is checking
the *planned managed target state*, not declaring legacy files healthy by fiat.
The normal live installer will instead call the same engine after component
commit and pass the validated Phase-10 build report so live-vs-DESTDIR identity
can be proven.

## Phase 14: required FX identity, rebuild triggers, and activation attestation

Phase 14 makes Realmheart FX a first-class Core-critical compatibility contract
rather than an optional plugin-shaped side effect. A supported production plan is
allowed only when the running Hyprland exposes a complete, clean commit + ABI
identity; `UNKNOWN` and `INCOMPATIBLE` FX states block the authoritative plan
before supported live mutation.

Each approved FX build receives a unique `fx_build_id` derived from the transaction,
manifest/source identity, and observed Hyprland identity. The installer passes that
identity plus the approved Hyprland commit/ABI into CMake. The plugin exposes the
same values through the read-only `hyprctl realmheart-fx identity` command, while
the installed loader is rendered from the approved plan instead of inheriting a
hard-coded compositor version.

The loader now verifies build identity even when Realmheart FX is already loaded.
A same-name plugin from an older build is rejected as stale instead of being
silently accepted. After a new load request, the loader also requires the plugin's
identity command to attest the expected build before reporting success.

Post-install verification distinguishes installation correctness from runtime
activation:

- plugin/loader existence and live-vs-validated-DESTDIR identity are install-health
  requirements;
- build provenance must match the approved Hyprland commit/ABI;
- fresh `hyprctl version -j` identity drift from the validated build is a
  Core-critical FX failure requiring rebuild;
- a loaded plugin whose `fx_build_id` differs from the validated build does **not**
  invalidate a correct on-disk installation; activation becomes
  `pending_session_restart`;
- an exact runtime build-ID/version/commit/ABI attestation promotes activation to
  `active` with runtime health `healthy`.

Receipt inputs now include a dedicated FX block with plugin hash, loader path,
build ID, Hyprland version/commit/ABI, and manifest-derived rebuild triggers. The
current trigger is `dep.hyprland.devel` with `version_commit_or_abi_change`, giving
future Doctor enough information to explain and schedule an FX rebuild after a
relevant Hyprland change.

Required FX failure is promoted to Realmheart Core **after** FX component
aggregation. This preserves the real root cause (`Realmheart FX: FAILED`) while
also preventing Core from being reported healthy, without creating a circular
manifest dependency.

## Phase 15: privacy-safe diagnostics, incident grouping, and report lifecycle

Phase 15 turns installer observations into one stable forensic contract instead of
asking users to paste raw terminal output or whole transaction directories into an
issue. Diagnostic reports are synthesized from the authoritative preflight/plan,
optional build-stage result, observed verification result, and rollback/recovery
availability.

The report schema deliberately contains an **allowlisted** environment summary:
distribution, architecture, kernel, package-manager kind, Wayland/systemd-user
availability, Hyprland version/compatibility/commit/ABI identity, and display
geometry. It never serializes the process environment, `HYPRLAND_INSTANCE_SIGNATURE`,
monitor description/serial strings, arbitrary command output, authentication data,
or private file contents. Known HOME/XDG/source/temp paths are normalized before
human-facing warnings/blockers are admitted to a report.

Verification failures use stable normalized `RH_*` codes derived from stable check
IDs. Causal grouping reports failed components as roots and computes transitively
blocked/affected components without repeating the same incident for every blocked
branch. The required-FX Core-critical bridge is treated as an effect of an FX root
failure rather than a second circular root cause. Build-stage failure can likewise
identify a failed BuildUnit root when verification never ran.

The incident fingerprint intentionally excludes transaction IDs, timestamps, user
names, and normalized private paths. Repeated occurrences of the same normalized
root failure against the same Realmheart/Hyprland contract therefore converge on
the same fingerprint while each stored occurrence still gets its own incident ID.

Generate a current-machine diagnostic without changing Realmheart installation or
configuration state:

```bash
python3 installer/realmheart_installer.py diagnose-current
python3 installer/realmheart_installer.py --json diagnose-current
```

A normal `diagnose-current` stores a private bundle under the installer reports
root with:

```text
report.json
report.md
github-issue.md
```

Use `--dry-run diagnose-current` to render the incident without persisting the
bundle. `--report-path <directory>` explicitly selects a different output
parent.

Managed report inspection/removal uses validated incident IDs rather than caller
supplied file paths:

```bash
python3 installer/realmheart_installer.py report-list
python3 installer/realmheart_installer.py report-inspect --report-id RH-DIAG-...
python3 installer/realmheart_installer.py report-remove --report-id RH-DIAG-...
```

Removal refuses traversal IDs, symlinked report directories/files, and report
folders containing unexpected foreign files. Phase 15 only *reports* rollback and
recovery availability; Phase 16 owns the final keep/restore decision and durable
installed-state receipt commit.

## Phase 16 — live transaction, final decision, and installed-state receipt

Phase 16 closes the installer transaction instead of stopping at a validated
staged payload. The explicit live entry point is:

```bash
python3 installer/realmheart_installer.py install
```

The default invocation remains plan-only, and `--dry-run install` still performs
no Realmheart installation mutation. A live install requires the authoritative
plan to be READY, performs permanent/pre-adoption/previous-version safety backup
actions first, executes the Phase-10 build and unprivileged DESTDIR staging,
commits the approved component/config/service footprint through the transaction
backend, runs observed-state Phase-13 verification, builds Phase-15 diagnostics,
and only then enters the final keep/rollback decision.

`--yes` bypasses only the initial package/live-install consent prompt. It never
silently accepts a degraded or failed installation. Interactive degraded/critical
results require a final choice; machine-readable callers may provide one
explicitly:

```bash
python3 installer/realmheart_installer.py --yes --decision keep install
python3 installer/realmheart_installer.py --yes --decision restore_previous install
python3 installer/realmheart_installer.py --yes --decision restore_baseline install
```

Without an explicit final `--decision`, JSON/non-interactive degraded or failed
runs prefer reversal of the current transaction rather than silently keeping a
bad state. Healthy verification is kept automatically.

Final states are deliberately distinct:

- healthy/success-with-warnings: keep and transactionally publish the observed
  schema-v2 `$XDG_STATE_HOME/realmheart/installed-state.json` receipt;
- healthy with `pending_session_restart`: keep the install, but receipt runtime
  health remains `unknown` until a fresh session proves the installed shell/FX;
- degraded: explicit keep or rollback; a kept receipt records `degraded` and the
  observed component/capability state honestly;
- Core/required-FX critical failure: rollback is recommended, but an explicit
  keep is supported and returns the dedicated critical-kept exit status;
- restore previous: reverse only this transaction's journaled live footprint and
  leave the previous managed receipt authoritative;
- restore permanent baseline: reverse this transaction, restore the validated
  pre-Realmheart baseline, and retire the prior managed receipt from the
  authoritative path while preserving its bytes in transaction preimages.

The live backend commits CMake-produced artifacts only from the validated
DESTDIR report. User configuration uses the existing exact/guarded transaction
primitives. Production privileged files use narrow `sudo` commands only for the
specific declared Realmheart target: existing `/usr/local`/`/etc` parent chains
must be real root-owned non-group/world-writable directories, missing
Realmheart-specific parents are created one level at a time, and rollback removes
transaction-created parents only with `rmdir` when still empty. The installer is
still run as the normal user; running the installer itself as root is refused.

Rollback reports do not overclaim global restoration. Package-manager changes
are intentionally conservative/best-effort and are reported as retained when the
Realmheart live transaction is reversed.

Live-system validation should initially be performed only through the
fake-root transaction tests and later disposable test users/VMs from the
real-system validation matrix. The existence of the `install` command is not a
recommendation to make a developer workstation the first destructive test host.


## Phase 17 — transactional uninstall

Phase 17 adds an uninstall path that is deliberately ownership-aware rather
than a recursive Realmheart delete:

```bash
python3 installer/realmheart_installer.py --dry-run uninstall
python3 installer/realmheart_installer.py --compare-config uninstall
python3 installer/realmheart_installer.py uninstall
```

The uninstall planner requires an authoritative kept `installed-state.json`,
validates receipt artifact paths against the current canonical manifest, loads
the permanent baseline, recovers last-managed fingerprints/service state/package
provenance from the installation transaction when available, and reports current
configuration divergence without printing private file contents.  A user-edited
receipt can therefore never authorize an arbitrary privileged deletion.

Configuration removal is an explicit choice:

- `keep-current` leaves the current Hyprland tree intact, removes only the
  Realmheart managed Kitty block, removes/restores exact Realmheart-owned
  drop-ins/artifacts, and never touches personal `config.fish`;
- `restore-baseline` first creates a pre-uninstall safety snapshot whenever the
  current configuration differs, then restores Hyprland through the same staged
  full-tree swap engine and restores baseline-owned shared files with
  compare-before-write guards.

For machine-readable execution, the choice must be explicit:

```bash
python3 installer/realmheart_installer.py --json --yes \
  --uninstall-config keep-current uninstall
python3 installer/realmheart_installer.py --json --yes \
  --uninstall-config restore-baseline uninstall
```

Managed user services are quiesced before their files are changed; failure to
stop/disable a known active Realmheart unit blocks cleanup rather than deleting
files from underneath it.  Pre-install service state is restored when a
pre-existing same-name unit is restored.  Privileged auth-helper/PAM cleanup is
restricted to canonical manifest paths and uses exact WAL-backed moves/restores.

Event Surface history at `$XDG_STATE_HOME/realmheart/events.db` is preserved by
default.  Removing it requires `--purge-event-history`.  The broader Matugen
`theme-palette.tsv` input is never owned by terminal uninstall.

Dependency cleanup is also opt-in.  `--cleanup-dependencies` considers only
packages whose historical provenance proves `installed_by_transaction=true`,
and the pacman adapter uses exact `pacman -R -- ...` removal rather than
recursive `-Rs`/`-Rns` expansion.  Packages pacman refuses to remove are retained
and reported.

Permanent baseline manifests created from Phase 17 onward record source
filesystem type, mode, UID and GID in addition to bytes/fingerprints.  This is
required for exact restoration of pre-existing privileged paths.  Older valid
baselines remain readable, but uninstall fails closed instead of guessing
privileged ownership/mode when those historical fields are absent.

## Phase 18 — crash recovery hardening and chaos validation

Phase 18 treats interruption/re-entry as a first-class transaction state instead
of assuming the process survives long enough to perform normal finalization.
Every mutating package/install/uninstall path allocates a small private recovery
reserve immediately before mutation begins. If recovery metadata itself hits
`ENOSPC`, the installer sacrifices that reserved space and retries writing the
transaction-local `recovery.json` report rather than losing the explanation of
why the transaction stopped.

Ctrl+C and SIGTERM are controlled interruptions. They mark a mutating transaction
`INTERRUPTED`, preserve the WAL, record the interruption reason, and write
`recovery.json` without attempting complicated asynchronous rollback from the
signal path. Hard process death is handled on the *next* invocation: startup
reconstructs durable `transaction.json` + `journal.jsonl` state before allowing a
new mutating transaction.

Recovery state has three deliberately conservative outcomes:

- `clean` — no transaction-owned recovery action remains;
- `recovery_available` — generic WAL state and current filesystem identity prove
  automatic rollback is mechanically safe;
- `manual_attention` — privileged/custom operations, package-manager uncertainty,
  missing preimages, malformed state, rollback failure, untrusted paths, or any
  other ambiguity prevents automatic repair.

A terminal `COMMITTED`/`ROLLED_BACK` summary does not override contradictory WAL
evidence. Incomplete journal operations force manual attention instead of being
silently blessed by the summary file.

The public recovery surface is:

```bash
python3 installer/realmheart_installer.py recovery-list
python3 installer/realmheart_installer.py \
  --transaction-id RH-... recovery-inspect
python3 installer/realmheart_installer.py \
  --transaction-id RH-... recovery-rollback
python3 installer/realmheart_installer.py \
  --transaction-id RH-... --yes recovery-acknowledge
```

`recovery-rollback` is intentionally narrow. Historical WAL paths are authorized
against freshly resolved trusted user roots before filesystem inspection, and
only generic journal operations with proven before/after identity can be reversed
automatically. System/privileged/service operations never gain authority merely
because a user-writable journal names a path. `recovery-acknowledge` performs no
repair; it records that an operator manually resolved or deliberately accepted a
manual-attention incident so future installs are not permanently blocked.

Dependency mutation gets its own durable pre-call marker. If the process dies
inside pacman before a `PackageInstallResult` can be recorded, the next startup
still knows an external BEST_EFFORT mutation may have occurred and refuses to
misclassify the abandoned transaction as clean.

The Phase-18 chaos suite failure-injects every full-tree swap window, WAL write
failures before/after filesystem mutation, disk exhaustion, missing preimages,
Ctrl+C/SIGTERM, hard process death and re-entry, package-manager failure/death,
verification failure, config drift after planning, receipt-write interruption,
rollback failure, malformed abandoned state, terminal-summary/WAL disagreement,
and tampered WAL paths. The acceptance rule is deliberately simple: no tested
failure path may leave unexplained state, and recovery must either produce a
safe deterministic rollback path or an explicit manual-attention report.

## Phase 19 — real-system validation matrix

Phase 19 does not add another live mutation mode to the installer.  It adds a
release-validation harness around the already-implemented installer so destructive
validation remains explicit, reproducible, and tied to evidence from disposable
users/VM snapshots.

The canonical matrix contains thirteen real-system scenarios from the implementation
plan: clean user, custom Hyprland, customized Kitty, customized Fish, non-default
XDG roots, pre-existing same-name Realmheart files/units, upgrade, same-version
reinstall, downgrade, missing soft dependency, multi-monitor topology,
intentionally broken component, and controlled Ctrl+C interruption.

The runner is intentionally separate from `realmheart_installer.py`:

```bash
python3 tools/validate-realmheart-installer.py list
python3 tools/validate-realmheart-installer.py guide clean-user
```

A Phase-19 validation ledger is private local evidence, not telemetry.  Create it
at a path that survives VM snapshot resets (for example a host-shared test-results
mount):

```bash
python3 tools/validate-realmheart-installer.py init \
  --report /path/to/phase19-report.json
```

Before destructive validation, run the fixture counterpart for the complete
matrix.  The harness maps every real-system row to existing installer
integration/chaos tests and runs every unique test only once:

```bash
python3 tools/validate-realmheart-installer.py fixture-matrix \
  --report /path/to/phase19-report.json \
  --evidence-dir /path/to/private-evidence
```

The fixture matrix is a prerequisite, not a substitute for the real-system
matrix.  A fixture PASS never marks the corresponding live scenario PASS.

Run the read-only host gate on each supported disposable host before live
mutation:

```bash
python3 tools/validate-realmheart-installer.py host-audit \
  --report /path/to/phase19-report.json \
  --evidence-dir /path/to/private-evidence
```

Host audit performs only:

- canonical manifest validation;
- `--dry-run install` (must be READY);
- `recovery-list` (must be clean).

Raw command output is stored as mode-0600 evidence files.  The JSON ledger keeps
only status/duration/hash metadata so it does not become a second verbose copy of
absolute HOME paths or environment output.

For each destructive scenario, reset/prepare the disposable environment according
to its guide, perform the test, save useful logs/screenshots/text evidence, and
record the result:

```bash
python3 tools/validate-realmheart-installer.py guide custom-kitty

python3 tools/validate-realmheart-installer.py record \
  --report /path/to/phase19-report.json \
  --scenario custom-kitty \
  --status pass \
  --note 'kitty.conf outside Realmheart markers remained byte-identical' \
  --evidence /path/to/custom-kitty.log \
  --evidence-dir /path/to/private-evidence
```

Evidence must be a regular non-symlink file.  Stored copies are mode 0600 and
recorded with SHA-256/size metadata.  Each live row also records a minimal host
summary and the source checkout revision/dirty state actually exercised; use
`--validated-source-root` when an upgrade/downgrade scenario intentionally runs a
different checkout.

The final release gate is deliberately strict:

```bash
python3 tools/validate-realmheart-installer.py summary \
  --report /path/to/phase19-report.json
```

Phase 19 is COMPLETE only when:

1. all thirteen fixture rows PASS;
2. the read-only host audit PASSes;
3. all thirteen real-system rows are explicitly recorded PASS.

`blocked` and `skipped` remain visible but do not satisfy the release gate.  The
validation runner never executes a destructive installation automatically; that
would violate the plan's requirement to use disposable users/VMs/snapshots and
would make a validation convenience script itself a new system-mutation footgun.

## Phase 20 — Installer ↔ Doctor forensic-contract validation

Phase 20 validates the persisted installer contract from the point of view of a
future maintenance/Doctor consumer.  Doctor remains a separate project: the
shared consumer lives in `realmheart_maintenance.forensics` and imports no
`realmheart_installer` handlers or mutation code.

The contract is deliberately three-way:

```text
canonical manifest (desired/current product contract)
              ↕
installed-state.json (last accepted installed observation)
              ↕
current health snapshot (later read-only observation)
```

`realmheart_maintenance.forensics` provides:

- strict, symlink-safe loading of schema-v2 `installed-state.json`;
- a schema-v1 read-only current-health snapshot shape;
- dependency, artifact, manifest-identity, activation, and runtime drift records;
- stable `RH_FORENSIC_*` error vocabulary;
- dependency-root incident collapsing with transitive affected components;
- an independent repair/build-readiness assessment so a healthy running
  Realmheart is not called unhealthy merely because build/repair tooling later
  disappeared;
- manifest-driven health-check selection honoring `contexts`, `cost`, and
  `side_effects` metadata.

The tiny synthetic consumer is:

```bash
python3 tools/validate-realmheart-doctor-contract.py \
  --receipt ~/.local/state/realmheart/installed-state.json \
  --snapshot /path/to/current-health.json
```

Use `--json` for machine-readable output.  A drift report is diagnostic data,
not a command to mutate the machine; the Phase-20 consumer performs no repair.
Malformed/newer schemas return exit 30 rather than guessing.

Phase-20 tests generate an installed-state receipt through the real installer
verification/receipt path and then feed that receipt to the standalone consumer.
A fresh subprocess asserts that no `realmheart_installer` package or handler was
loaded by the consumer.  Fixtures separately prove dependency-vs-artifact drift,
runtime-health-vs-repair-readiness, health-check context/cost policy, and one
shared DBus root incident collapsing multiple affected Realmheart components.

## Post-Phase-20 release integration — Doctor Acceptance MVP

The public installer now bundles a deliberately small read-only Realmheart Doctor
acceptance layer. This is **not** the full temporal/repair Doctor roadmap. It exists
only to provide an independent second opinion between observed installer
verification and the final keep/rollback decision.

The live sequence is:

```text
live commit
  -> installer verification
  -> doctor-candidate.json (not accepted/kept state)
  -> read-only Doctor artifact/dependency re-observation
  -> doctor-assessment.json
  -> final keep/rollback decision
  -> durable installed-state receipt if kept
```

Doctor recommendations are `keep`, `keep_with_warnings`, `revert_recommended`,
or `indeterminate`. `revert_recommended` escalates an otherwise healthy installer
result to an explicit final choice; it never performs rollback itself. If Doctor
raises or cannot establish a verdict, the installer records `indeterminate` and
continues using its own verification evidence rather than turning Doctor into a
single point of failure.

`pending_session_restart` is a valid acceptance state. The deployment may be kept
while runtime health remains unknown until a fresh Hyprland session can establish
Last Known Good later.

The kept schema-v2 installed-state receipt records the Doctor acceptance result so
future maintenance tooling can distinguish installer evidence, Doctor evidence,
and the final accepted state.
