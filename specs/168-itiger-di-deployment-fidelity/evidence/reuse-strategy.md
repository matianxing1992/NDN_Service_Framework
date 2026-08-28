# T010 — PreSplitFirstStrategy reuse and placement evidence

## Verdict

PASS for the local strategy gate. This evidence does not claim that the
TigerCluster 30-row cold/warm campaign (T011) has run.

## Decision contract

`PreSplitFirstStrategy` now evaluates only ACK-closed, graph-valid and
capacity-feasible Provider candidates. Its deterministic priority is:

1. exact pinned GPU shard;
2. exact reload-safe GPU shard;
3. exact host-RAM shard;
4. exact disk shard;
5. an active, content-addressed Repository/catalog candidate;
6. a newly generated graph-valid split.

Within the same residency tier, the score uses estimated wait, queue depth,
RTT, bandwidth, reusable-state savings, already assigned GPU memory, and the
Provider name as the final deterministic tie-breaker. Cache reuse is rejected
when model content, semantics, graph, partition, precision, backend, boot
epoch, cache epoch, capture age, expiry, pin lifetime, or active Repository
catalog identity does not match.

The placement evidence retains a compact exact ACK set using each Provider's
signed offer/evidence digests, resource sequence and boot epoch, plus the
capability fields used by the decision. Each role retains every feasible or
rejected candidate, its rejection reason or score components, and the selected
candidate. No model payload, token, key, or cache bytes are copied into the
decision evidence.

## Focused verification

Command:

```text
python3 tests/python/test_ndnsf_di_presplit_first_strategy.py
```

Result: 11 tests passed. Covered cases include the complete residency ordering,
published-before-generated selection, false/stale/partition-mismatched cache
rejection, first-publication safety, equal-capacity balanced placement,
heterogeneous-capacity placement, unknown runtime-bound rejection, reusable
derived state, exact ACK-set retention, and wait/queue/RTT/bandwidth score
evidence.

Related regression suites also passed:

```text
tests/python/test_ndnsf_di_presplit_catalog.py                 3
tests/python/test_ndnsf_di_automatic_collaboration_plan.py    15
tests/python/test_ndnsf_di_placement_strategy.py               7
tests/python/test_ndnsf_di_external_placement_strategy.py      4
tests/python/test_spec168_cache_residency.py                    4
tests/python/test_spec168_deferred_planning.py                  7
```

T011 remains the next gate: the frozen five-prompt schedule must execute in one
unchanged TigerCluster allocation and prove warm reuse through identity-bound
zero duplicate Repo bytes and zero redundant device loads, not latency alone.
