# Spec 136 Formal Evidence Freeze

**Frozen**: 2026-07-23 CDT  
**Canonical campaign**:
`results/spec136-rsa-single-worker/formal-r6-20260723T224836Z`  
**Status**: `TRADE_OFF`, 10/10 cells executed exactly once

Spec 136 and its formal MiniNDN matrix are closed evidence. The campaign MUST
NOT be rerun, selectively supplemented, replaced, tuned, or reinterpreted by
discarding the heartbeat regression or the four rates that did not meet the
registered usefulness threshold.

The frozen conclusion is:

- both modes sustained 200–400 pps/peer with 100% measured delivery;
- only 400 pps/peer met `USEFUL_AT_RATE`, through a 33.71% delivery-p99 gain;
- that same pair had a 70.96% heartbeat-p99 regression;
- SC-006 was not met, so `ASYNC_SINGLE_WORKER_USEFUL` is not authorized;
- moving Data publication preparation off Face does not accelerate RSA itself.

Permitted actions:

- read and analyze the canonical artifacts;
- verify hashes, receipts, and the sealed manifest without launching a cell;
- cite the formal `TRADE_OFF` result and its audit limitations;
- define a later Spec with a new runner, manifest, result namespace, and
  acceptance decision.

The post-implementation audit's missing FR-015 distributions/CPU/Face-residence
metrics and worker-specific fault injection are frozen limitations. They cannot
be repaired by rerunning or selectively adding Spec 136 cells.
