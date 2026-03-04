# Waybeam Hub C/Python Sync Roadmap (Draft)

## Goals
- Keep `waybeam_hub.py` (groundstation) and `waybeam_hub.c` (embedded/vehicle) behavior aligned.
- Add a lightweight WebUI path for both runtimes for setup/config sessions.
- Define a common protocol for exchanging telemetry, menu state, and control intents across multiple running instances.

## Phase 1: Compatibility Baseline
1. Normalize shared concepts:
   - source names (`serial`, `joystick`)
   - menu state shape (`current_section`, `selected`, `menu_visible`, `status`)
   - command verbs (`menu`, `up`, `down`, `select`)
2. Add a parity checklist used on every release:
   - config key coverage
   - HTTP endpoint coverage
   - radio-rule trigger semantics
3. Maintain one changelog section documenting py/c drift and resolution.

## Phase 2: Common Sync Protocol (Push/Pull)

### Transport
- Primary: UDP multicast or unicast gossip on local network for low overhead.
- Fallback: HTTP pull endpoint on each node.
- Message format: JSON lines (versioned schema).

### Identity and Session
- Each instance has:
  - `instance_id` (stable UUID)
  - `runtime` (`py` or `c`)
  - `role` (`vehicle`, `groundstation`, `observer`)
  - `boot_id` and monotonic sequence (`seq`)
- Single active editor lock for config/setup window:
  - lease owner + lease expiry timestamp.

### Message Types
1. `hello` (push): announces presence and capabilities.
2. `telemetry` (push): channel values, source health, link status.
3. `menu_state` (push): current menu position, selected entry, overlay visible.
4. `state_delta` (push): compact updates (asset toggles, active source changes).
5. `command_intent` (push): requested action (menu nav or action launch).
6. `snapshot_request` (pull trigger): ask peer for full state.
7. `snapshot_response` (pull response): complete current state for reconciliation.
8. `ack` (push): acknowledges `command_intent` or config mutation.

### Push/Pull Rules
- Push:
  - send `telemetry` at fixed interval (e.g. 5–10 Hz)
  - send `state_delta` and `menu_state` on change
  - send `ack` for any accepted command
- Pull:
  - on startup, request snapshot from preferred peer
  - on sequence gap or stale peer timeout, issue pull request
  - periodic low-rate pull (e.g. every 5s) to correct missed updates

### Conflict Resolution
- Last-writer-wins by `(epoch, seq, instance_id)` for non-critical state.
- Command path requires lease owner OR explicit `force` flag.
- Reject stale seq and duplicate IDs idempotently.

### Reliability / Safety
- Sequence tracking per peer.
- Heartbeat timeout marks peer stale.
- Backoff reconnect behavior.
- Strict command allowlist (no raw shell over sync channel).

## Phase 3: Implementation Plan
1. Write protocol schema doc (`protocol/v1.md`).
2. Implement serializer/deserializer in Python first.
3. Implement fixed-buffer parser/encoder in C.
4. Add bridge adapters:
   - py: SSE + WebUI -> protocol events
   - c: SSE + WebUI -> protocol events
5. Add replay log mode for debugging drift.

## Phase 4: Verification
- Golden message fixtures shared by py/c.
- Cross-runtime simulation:
  - py -> c command flow
  - c -> py state sync
  - packet loss + out-of-order handling
- Manual setup test with one vehicle and one groundstation instance.

## Open Questions
- Multicast availability on all target embedded deployments?
- Should lease coordination be centralized or peer-elected?
- Do we need binary encoding for constrained links in v2?
