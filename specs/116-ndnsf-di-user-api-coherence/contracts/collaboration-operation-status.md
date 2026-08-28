# Contract: Generic NDNSF Collaboration Operation Status

## Boundary

NDNSF Collaboration owns reusable progress inspection for long-running selected
work. It does not know models, Qwen, shards, adapters, GPUs, deployment
revisions, or inference readiness. NDNSF-DI supplies those meanings as bounded
application details and validates its own readiness receipts.

No new Request/ACK/Selection/Response kind and no parallel Targeted status
service are introduced. The existing provider-scoped `SELECTION-STATUS` query
and `SelectionExecutionStatus` reply remain the wire and lifecycle owner.
`addCollaborationHandler` makes that existing reply queryable, and
`RequestCollaboration` records selected Provider/digest bindings needed by the
query. This default wiring replaces today's separate, easy-to-miss
`setSelectionStatusQueryable` plus tracked-request setup.

## Additive Snapshot

`SelectionExecutionStatus` retains its existing fields and adds zero or more
latest generic member entries:

```text
CollaborationOperationStatusSnapshot
  provider_name, service_name, request_id, selection_digest
  status_epoch, observed_at, expires_at
  member_statuses: tuple[ServiceOperationStatus, ...]
  signer_key_id, signature/evidence

ServiceOperationStatus (additive fields shown)
  operation_id, operation, role/member, attempt, sequence
  state, progress_known, progress, reason_code, message
  created_at, updated_at, expires_at, retry_after
  details_schema?, details_payload?  # bounded, non-secret APP observation
```

The tuple is bounded and deterministic by `(role, operation_id)`. Existing
line-oriented status readers ignore appended `member_status_count` and
indexed, canonical length/base64 member entries; new readers accept both the
legacy base reply and the extended form. A Provider
stores only the latest accepted status per key plus bounded terminal retention.
An update with a lower epoch/attempt/sequence, mismatched binding, invalid
transition, expired timestamp, oversized details, or post-terminal regression
is rejected. `progress_known=false` distinguishes “unknown” from a real 0.0
value because many operations cannot estimate a meaningful percentage.

## Provider API

The API must cover both sides of handler construction:

```cpp
provider.reportCollaborationOperationStatus(binding, status); // pre-context
ctx.reportOperationStatus(status);                             // in handler
```

```python
provider.report_collaboration_status(binding, status)
ctx.report_status(status)
```

The pre-context reporter is required because
`prepareCollaborationAssignmentAsync(...)` fetches keys/artifacts before the
runtime constructs `CollaborationContext`. Both calls update the same snapshot;
neither creates a new operation store.

## Requester API

The existing selection-status query is exposed through a typed low-level
`ServiceUser` Python binding:

```python
snapshot = user.query_collaboration_status(binding)
for update in user.watch_collaboration_status(binding, after=cursor): ...
snapshot = user.wait_for_collaboration_status(binding, predicate, timeout=...)
```

The NDNSF-DI request handle keeps its domain-facing `deployment_status()` and
`events()` methods and uses this low-level API internally; it does not expose a
second generic status vocabulary to normal inference callers. `watch` may use
optional collaboration notifications but must reconcile with
the latest query snapshot. `wait` is bounded local observation and does not
change the distributed request deadline. Loss, duplication, reordering, a
requester restart, or absence of collaboration events cannot make the latest
queryable status unreachable while retained.

## Trust and Authority

- Validate Data signature and selected Provider identity before parsing status.
- Bind request, service, selection digest, Provider, role/member,
  attempt/status epoch, and sequence; reject cross-request or replayed entries.
- Status is observational. It cannot replace Selection responsibility,
  execution certificates, leases, readiness receipts, commit, or final
  Response.
- Application details are size-bounded and must not expose model credentials,
  raw tokens, encryption keys, or private paths. Sensitive detail uses an
  authorized encrypted reference outside this envelope.
- Unknown/legacy Provider replies remain representable as base
  `SelectionExecutionStatus`; absence of member progress is not READY.

## NDNSF-DI Mapping

NDNSF-DI emits operation `ndnsf-di.prepare-role` and maps its domain phase into
the bounded detail payload:

```text
generic RUNNING + DI phase FETCHING / VERIFYING / LOADING / WARMING
generic DONE    + DI phase READY only after exact ProviderReadiness exists
generic FAILED/CANCELED/EXPIRED + typed DI reason
```

The coordinator aggregates entries into `DeploymentStatus`, then separately
validates exact `ProviderReadiness` for every required role. A generic `DONE`, a
progress value of `1.0`, or a DI `READY` detail alone never opens the model
runner barrier.

## Acceptance

Contract/unit and MiniNDN tests must cover pre-context reporting, context
reporting, multi-role snapshots, query/watch/wait, legacy empty snapshots,
event loss/reorder/duplication, requester restart, Provider restart/epoch
change, expired/forged/cross-role/replayed status, post-terminal regression,
bounded retention/details, and proof that generic DONE cannot bypass DI exact
readiness.
