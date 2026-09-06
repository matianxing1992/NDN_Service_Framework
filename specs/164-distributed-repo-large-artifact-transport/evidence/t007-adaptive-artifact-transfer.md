# T007 Adaptive Artifact Transfer Evidence

## Verdict

PASS for the T007 implementation boundary. The repository now has one
NDNSF collaboration invocation for replica/lease selection and a separate,
bounded segmented-NDN data plane. Local deterministic tests cover scheduling,
retransmission, duplicate suppression, backpressure, lease validation, and the
control-count invariant. Successful live NDN transfer and atomic end-to-end
publication are intentionally reserved for T008.

## Implemented Contract

- `ReplicaLeaseCollaborationClient.begin()` opens exactly one delayed-planning
  NDNSF collaboration request.
- ACK closure is authoritative before lease selection.
- `commit_leases()` accepts only providers with successful ACKs and emits one
  bound Selection assignment per chosen replica.
- Rejected NDNSF plan commits leave the local control state at `ACK_CLOSED`;
  the state cannot falsely advance to `PLAN_COMMITTED`.
- `ReplicaLeaseControlFlow` counts one Request plus one Selection per replica.
  The count has no segment-count or artifact-size input.
- `AdaptiveArtifactTransfer` implements a bounded AIMD window, bounded
  verification backlog, timeout/retry budget, out-of-order receipt, duplicate
  suppression, and fail-closed retry exhaustion.
- `fetch_adaptive_segmented_data_packets()` is the live NDN consumer binding.
  It derives the versioned segmented name from the first Data packet, pipelines
  Interests, validates segment/name continuity, and synchronously invokes the
  caller's packet callback to provide verification/persistence backpressure.
- `NativeRepoDataPlaneProducer` remains the bounded exact-wire producer; the
  control path does not invoke one service operation per segment.

## Security and Resource Boundaries

- Upload lease assignments carry the complete artifact identity, repository
  identity, reservation, expiry, operation identifier, and replay identifier.
- Expired leases, duplicate repository/lease selection, selection outside the
  successful ACK set, malformed assignment payloads, and invalid scheduler
  bounds fail closed.
- Root-manifest trust and per-chunk digest verification are not duplicated in
  the fetcher. The synchronous packet callback must apply the T005 manifest
  verifier before calling `mark_verified()` or committing T006 storage.
- The live fetcher never assembles the complete artifact in memory. Its memory
  is bounded by the congestion window, verification backlog, scheduler state,
  and one callback packet at a time.

## Verification

```text
./build/unit-tests -t 'DistributedRepoArtifactTransfer/*' -l all
  5/5 PASS

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec164_artifact_transfer.py
  5/5 PASS

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec164_streaming_artifact_store.py
  5/5 PASS

python3 -m py_compile \
  NDNSF-DistributedRepo/pythonWrapper/py_repoclient/artifact_transfer.py \
  tests/python/test_spec164_artifact_transfer.py
  PASS

python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  --strict --json specs/164-distributed-repo-large-artifact-transport
  PASS: no blockers or warnings
```

The core and repository Python extensions compiled successfully with the
resource-bounded verification profile:

```text
CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 setup.py build_ext --inplace --force -j1
```

The default core-extension `-O2 -g` compilation was killed by the constrained
local host. This is recorded as a build-resource limitation, not hidden as a
functional pass or treated as a protocol failure.

## Gate Record

- Context Mode: anomaly statistics collected; repository guard failed closed
  because no project ContentDB was bound. Repository documents and live source
  were used as authority.
- CodeGraph: verified the collaboration adapter, atomic state transition, and
  test blast radius from current source.
- Spec Kit: strict structural audit passed for all 44 requirements and 20
  tasks.
- GSD: installation health passed; the unrelated phase-34 missing-summary
  informational item remains unchanged.
- ARS: not applicable to this frozen-contract implementation task. It becomes
  applicable for T008 experiment design.

## Claim Boundary

This evidence does not claim a successful packet transfer over a local NFD or
MiniNDN, authenticated publication receipt, activation, or atomic consumer
destination. Those are T008 acceptance obligations.
