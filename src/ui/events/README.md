# Realmheart Event Surface — Producer Guide

This directory contains the GTK renderer for Realmheart Event Surface. The
renderer is intentionally generic: **the producer supplies event data and
Realmheart decides how to present it**.

The public ingress point is **not** `EventSurface.cpp`. Producers talk to the
background daemon, `realmheart-eventd`, either through the `realmheart-event`
CLI or directly over its Unix socket.

Any same-user local service can publish events. It does **not** need to be a
Realmheart component, link Realmheart libraries, use C++, or know anything
about GTK.

## Fastest path: use `realmheart-event`

After a normal Realmheart build, `realmheart-eventd` is installed/enabled as a
systemd user service and the freshly built daemon is started/restarted in the
background automatically.

Check it with:

```bash
systemctl --user status realmheart-eventd.service
./build-hybrid/realmheart-event status
```

For an installed build, use `realmheart-event` from `PATH` instead of the
`./build-hybrid/...` path.

Send a minimal event:

```bash
realmheart-event send \
  --id backup.current \
  --source user.backup-service \
  --title "Backup started" \
  --summary "Backing up the home directory" \
  --severity info \
  --presentation attention \
  --persistent
```

Canonical identity is:

```text
(source_id, event_id)
```

Sending/updating the same identity changes the existing card instead of
creating a duplicate.

Update it:

```bash
realmheart-event update backup.current \
  --source user.backup-service \
  --progress 0.64 \
  --progress-label "64%" \
  --field "Files=18,492" \
  --field "Size=12.4 GiB"
```

Resolve it:

```bash
realmheart-event resolve backup.current \
  --source user.backup-service \
  --title "Backup complete" \
  --severity success
```

Delete it entirely:

```bash
realmheart-event delete backup.current --source user.backup-service
```

Dismissal is different from resolution: dismissal removes an event from the
active surface but retains it in history.

```bash
realmheart-event dismiss backup.current --source user.backup-service
```

## Event fields available to producers

A v1 event may provide:

- `id`
- `source.id`, `source.name`, `source.icon`
- `severity`: `info`, `success`, `warning`, `critical`
- `presentation`: `ambient`, `attention`, `persistent`
- `title`
- `summary`
- ordered structured `fields`
- `progress`: none, indeterminate, or determinate `0.0..1.0`
- bounded plain-text `details`
- ordered `actions` (up to 8)
- `lifecycle.persistent`

The producer controls content, field order, action count/order/labels, progress,
and lifecycle updates. Realmheart controls the visual layout, theme, spacing,
animations, and safety policy.

## JSON input

For advanced producers, JSON is the cleanest interface.

`event.json`:

```json
{
  "id": "build.realmheart.main",
  "source": {
    "id": "example.build-service",
    "name": "Example Build Service",
    "icon": "applications-development"
  },
  "severity": "warning",
  "presentation": "persistent",
  "title": "Build needs attention",
  "summary": "Two targets failed while compiling.",
  "fields": [
    {"label": "Target", "value": "realmheart"},
    {"label": "Failures", "value": "2"}
  ],
  "progress": {
    "mode": "determinate",
    "value": 0.73,
    "label": "73%"
  },
  "details": {
    "format": "plain",
    "text": "bounded log or diagnostic text"
  },
  "actions": [
    {
      "id": "docs",
      "label": "Open documentation",
      "kind": "uri",
      "uri": "https://example.com/docs"
    },
    {
      "id": "copy",
      "label": "Copy diagnostics",
      "kind": "copy",
      "value": "diagnostic payload"
    },
    {
      "id": "retry",
      "label": "Retry",
      "kind": "registered"
    }
  ],
  "lifecycle": {
    "persistent": true
  }
}
```

Send it:

```bash
realmheart-event send --json event.json
```

Or stream generated JSON through stdin:

```bash
my-service-generate-event | realmheart-event send --json -
```

`--json` may contain either the event object shown above or a complete protocol
envelope containing `protocol` and `op`.

### Calling it from another systemd user service

External user services may explicitly order themselves after Event Surface if
they want the daemon guaranteed to be available before their first event:

```ini
[Unit]
Wants=realmheart-eventd.service
After=realmheart-eventd.service

[Service]
Type=simple
Environment=REALMHEART_EVENT_SOURCE=com.example.worker
ExecStart=/path/to/example-worker
```

The worker can then invoke `realmheart-event` itself, or connect directly to the
socket described below. Use the absolute path to `realmheart-event` from a user
service if its systemd `PATH` does not include the installed binary.

## Producer-defined buttons

Actions render in the exact order supplied by the producer.

### URI

```json
{"id":"docs","label":"Open docs","kind":"uri","uri":"https://example.com"}
```

Allowed v1 URI schemes are `https`, `http`, `file`, and `realmheart`.

### Copy

```json
{"id":"copy","label":"Copy error","kind":"copy","value":"E_FAIL ..."}
```

Clipboard mutation only occurs after the user clicks the button.

### Registered callback

```json
{"id":"retry","label":"Retry","kind":"registered"}
```

A registered action routes the click back to the producer. A long-lived service
must keep an action-listener connection open for its source ID.

For testing from a shell:

```bash
realmheart-event listen-actions --source example.build-service
```

When `Retry` is clicked the listener receives a framed JSON message like:

```json
{
  "protocol": 1,
  "type": "ACTION_INVOKED",
  "invocation_id": 7,
  "source_id": "example.build-service",
  "event_id": "build.realmheart.main",
  "action_id": "retry"
}
```

There is deliberately **no shell-command action type**. The producer receives
the callback and decides what `retry` means in its own process/security model.

## Direct socket protocol — no Realmheart library required

Services written in Python, Rust, Go, C, Java, etc. may skip the CLI entirely.
Native Realmheart components may also reuse `src/events/EventClient.*`; third-party
programs do not need that library because the wire format is deliberately
language-agnostic.

Socket:

```text
$XDG_RUNTIME_DIR/realmheart/eventd.sock
```

Fallback when `XDG_RUNTIME_DIR` is unavailable:

```text
/tmp/realmheart-<uid>/eventd.sock
```

Transport is an `AF_UNIX` stream. Every frame is:

```text
[4-byte unsigned big-endian JSON length][UTF-8 JSON payload]
```

Maximum frame size is 1 MiB.

Minimal create envelope:

```json
{
  "protocol": 1,
  "op": "create",
  "event": {
    "id": "health.current",
    "source": {
      "id": "com.example.healthd",
      "name": "Example Health Service"
    },
    "title": "Service degraded",
    "severity": "warning",
    "presentation": "attention"
  }
}
```

Tiny Python example using only the standard library:

```python
import json
import os
import socket
import struct

runtime = os.environ.get("XDG_RUNTIME_DIR")
path = (
    f"{runtime}/realmheart/eventd.sock"
    if runtime
    else f"/tmp/realmheart-{os.getuid()}/eventd.sock"
)

message = {
    "protocol": 1,
    "op": "create",
    "event": {
        "id": "worker.current",
        "source": {
            "id": "com.example.worker",
            "name": "Example Worker",
        },
        "title": "Worker started",
        "summary": "This event did not use Realmheart libraries.",
        "severity": "info",
        "presentation": "attention",
    },
}

payload = json.dumps(message, separators=(",", ":")).encode("utf-8")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(path)
    sock.sendall(struct.pack("!I", len(payload)) + payload)

    size = struct.unpack("!I", sock.recv(4))[0]
    response = b""
    while len(response) < size:
        response += sock.recv(size - len(response))

print(json.loads(response))
```

Production code should use a small `recv_exact()` helper because `recv(4)` is
not guaranteed to return all four bytes in a single call.

## Security / who may publish

Event Surface is intentionally a **local same-user IPC service**.

- no API key is required;
- the socket and runtime directory are owner-only;
- Linux peer credentials are checked with `SO_PEERCRED`;
- a process from another UID is rejected;
- generic same-user scripts and daemons are accepted;
- merely choosing a `realmheart-*` source ID does not grant Realmheart trust;
- arbitrary shell commands, GTK widgets, CSS, HTML, and producer-owned UI code
  are not accepted.

This means a user's backup daemon, development tool, metadata scrubber, cron-like
user service, or third-party desktop utility can all publish to Event Surface,
while another system user cannot inject cards into the session.

## Persistence and history

`lifecycle.persistent: true` means an active event is restored if
`realmheart-eventd` itself restarts.

SQLite state lives at:

```text
$XDG_STATE_HOME/realmheart/events.db
```

or, normally:

```text
~/.local/state/realmheart/events.db
```

Useful diagnostics:

```bash
realmheart-event status
realmheart-event list
realmheart-event history --limit 50
realmheart-event sources
realmheart-event inspect EVENT_ID --source SOURCE_ID
```

## Background service and builds

`realmheart-eventd` is independent from the GTK renderer. The shell may restart
without killing daemon-owned event state.

A normal source-tree build has `REALMHEART_EVENTD_AUTOSTART=ON` by default. When
the `realmheart-eventd` target is relinked, Realmheart:

1. writes/refreshes `~/.config/systemd/user/realmheart-eventd.service`;
2. enables it under the systemd user `default.target`;
3. starts it if it is not running;
4. restarts it after a rebuild so the new daemon binary is actually live.

Check logs/status with:

```bash
systemctl --user status realmheart-eventd.service
journalctl --user -u realmheart-eventd.service -f
```

Disable build-time service management when packaging/experimenting:

```bash
cmake -S . -B build-hybrid -G Ninja \
  -DREALMHEART_EVENTD_AUTOSTART=OFF
```

or for one build invocation:

```bash
REALMHEART_EVENTD_AUTOSTART_DISABLE=1 cmake --build build-hybrid
```

In CI, containers, or other environments without a live systemd user manager,
the autostart helper safely skips service management instead of failing the
compile.

## Protocol details and limits

The complete v1 wire contract, validation limits, rate limits, history behavior,
trust classes, subscriber messages, and action semantics live in:

```text
docs/events/protocol-v1.md
```

The important boundary is simple:

> Producers describe events. Realmheart decides presentation.
