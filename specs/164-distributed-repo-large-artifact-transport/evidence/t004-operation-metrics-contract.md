# T004 Operation Metrics Contract Evidence

## Result

PASS. Native and Python operation metrics share one JSON schema and reject
malformed identities, phases, time boundaries, counters, and replica-receipt
relationships.

## Stable Contract

- Operation identity: non-empty caller-visible UTF-8, at most 256 encoded
  bytes, with no control characters.
- Lifecycle phases: discovery, reservation, transfer, verification,
  persistence, replication, commit, and activation only.
- Byte accounting: logical payload, wire, retransmitted, payload-store read,
  and payload-store written bytes are independent counters.
- Cryptographic work: asymmetric and digest verification counts and durations
  are separate.
- Scaling evidence: control operations, metadata operations, and durable
  metadata record cardinality are separate.
- Durability: requested, selected, committed, and rejected receipt counts are
  separate; committed means distinct retained valid receipts.
- Replica invariant:
  `committedReplicaCount <= selectedReplicaCount <= requestedReplicaCount`.

The Python network client now creates one thread-local metrics record per
operation, returns its stable identity, records control calls and canonical
phase timings, and finalizes the same record at operation completion.

## Verification

```text
./waf build --targets=unit-tests,ndnsf-distributed-repo -j2
  PASS

build/unit-tests --run_test=DistributedRepoOperationMetrics
  2/2 PASS

python3 setup.py build_ext --inplace
  PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_operation_metrics.py
  5/5 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_persistence_authority.py
  6/6 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_ha.py
  48/48 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_exact_packets.py
  12/12 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_tiered_cache.py
  11/11 PASS

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_chunked_file.py
  2/2 PASS
```

## Workflow Gates

- Context Mode: `ctx_stats` completed; guard health failed because no project
  ContentDB was bound to `.specify/feature.json`. Repository documents were
  used as the authoritative fallback.
- CodeGraph: current `RepoOperationStatus`, Python client metrics, call flow,
  and test blast radius were inspected before edits.
- Spec Kit: prerequisites passed; requirements checklist was 16/16 complete;
  T004 stayed within the T005 trust-composition boundary.
- GSD: local installation health validation recorded at closeout.
- ARS: not applicable because this task implements an already frozen metrics
  contract and makes no literature, paper, or statistical-inference claim.
