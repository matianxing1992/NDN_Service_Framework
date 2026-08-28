# T003 Canonical Lifecycle Evidence Gate

**Status**: PASS for the focused canonical-contract and journal gate. No
MiniNDN process, container, model preparation, or TigerCluster job was started.

## RED-first result

The new focused test initially failed at import time because
`InvocationSummaryV1` and `LifecycleEventV1` did not exist. This established
that the gate exercised new production behavior rather than a pre-existing
mock path.

## Implemented boundary

- `LifecycleEventV1` is canonical JSON with all common fields present,
  immutable request/attempt/plan/Provider-boot/role/operation bindings, and a
  bounded lifecycle vocabulary.
- Generic `TIMEOUT`/`ERROR` terminal labels are rejected. A failure must use an
  exact taxonomy family such as `REPO_FETCH_PROGRESS_STALLED`, or the explicit
  `UNRESOLVED_EVIDENCE_GAP` when evidence cannot support a boundary.
- `RuntimeJournal.append_lifecycle_event()` validates and writes one decision
  while holding the journal's exclusive lock. Unauthenticated, stale,
  non-monotonic, wrong-plan, wrong-role, duplicate, and post-terminal events are
  retained with a rejection code but cannot advance subsequent state.
- The first accepted `RESPONSE_PUBLISHED`, `FAILURE_PUBLISHED`, `CANCELED`, or
  `EXPIRED` event owns the terminal state.
- `InvocationSummaryV1` contains the complete terminal, answer/token, phase
  latency, Repository byte, device-load, CPU-fallback, security, and exact
  failure-code fields required by the Spec 168 evidence schema.
- `reconcile_frozen_schedule()` fails closed on missing, duplicate, extra,
  prompt-mismatched, request-reused, or analyzer-rejected rows and classifies
  every frozen row exactly once.

The journal records a verified caller's `authenticated` decision; cryptographic
signature/token verification remains at the NDNSF-DI ingress that constructs
the event. Later integration tasks must not construct an accepted event before
that ingress check succeeds.

## Verification

The final direct-unittest gate used the repository root and explicit Python
paths so both `Experiments` and the NDNSF-DI package resolve identically:

```text
PYTHONPATH=.:NDNSF-DistributedInference python3 <nine test entrypoints>
Spec 168 lifecycle evidence                 3 passed
Runtime journal                            20 passed
Core contracts                              6 passed
Runtime v1                                 26 passed
Lifecycle history                           3 passed, 36 exhaustive histories
Deployment workflow                        10 passed
User journey                                5 passed
Spec 129 selection-gated core               8 passed
Spec 165 lineage                            4 passed
Total                                      85 passed
```

Additional checks:

```text
python3 -m py_compile <three production modules> <focused test>  PASS
git diff --check -- <T003 files>                                PASS
```

One preliminary direct invocation of `test_spec165_lineage.py` omitted the
repository root from `PYTHONPATH` and failed to import `Experiments`. Re-running
the unchanged test with the project-standard explicit path passed 4/4; this was
a test-command environment error, not an NDNSF-DI logic failure.

## Admission boundary

T003 does not prove real NDN deployment fidelity and does not authorize remote
execution. T004 must next prove that the lifecycle is emitted by real MiniNDN
processes and by the exact candidate container, without a host-NFD substitute,
fixed settle sleep, or mocked transport/GPU success.
