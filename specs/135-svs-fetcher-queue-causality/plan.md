# Implementation Plan: NDN-SVS Fetcher Queue Causality Diagnostic

**Branch**: `135-svs-fetcher-queue-causality` | **Date**: 2026-07-23 | **Spec**: [spec.md](spec.md)

## Summary

Create one isolated diagnostic descendant of the frozen Spec 133 profiled
subject. Configure and prove RSA-2048 Data signing, expose the Fetcher window
only inside that worktree, add a driver option for bounded piggyback capacity,
and reuse the verified MiniNDN route procedure under a new runner/result
authority. Execute a fresh five-rate RSA sweep, seal its first instability
boundary, then complete a preregistered 2x2 factor intervention with three
additional cells. Analyze mechanism evidence without changing production
NDN-SVS.

## Technical Context

**Language/Version**: C++17, Python 3.8

**Dependencies**: NDN-SVS profiled head `e9913c9`, ndn-cxx, Boost 1.71,
MiniNDN, NFD

**Storage**: Hash-bound JSON manifests/receipts, JSONL events/resources, NDN
profile logs, CSV/Markdown analysis

**Testing**: Source-contract tests, RSA wire-type assertion, build/linkage
audit, binary self-test, eight once-only MiniNDN diagnostic cells, analyzer
fixtures

**Target Platform**: Existing Linux MiniNDN host, CPUs 0 and 2

**Constraints**: Spec 133 immutable; single-I/O thread; 10/60/10 timing; five
sweep plus three treatment cells only; no retries; no production fix

## Constitution Check

- **CodeGraph/source reality**: Spec 133 established that `Fetcher` owns a
  FIFO queue and a fixed `m_windowSize = 10`; `updateCallbackInternal()` expands
  missing ranges and `fetchAll()` scans pending entries.
- **Spec-driven**: Spec 135 owns all diagnostic changes and evidence.
- **Right validation scope**: MiniNDN, 60-second measurement, two real peer
  processes, verified routes.
- **Security boundary**: HMAC Sync Interests, RSA-2048 Data, validators
  disabled. RSA Data signing deliberately differs from the Spec 133
  `DigestSha256` baseline and matches the deployment assumption; no NDNSF
  security path is involved.
- **Task cohesion**: Implementation/tests are one behavior; once-only campaign
  execution is a separate irreversible task.

**Gate**: PASS, subject to the pre-implementation audit.

## Experimental Design

### Research question

Where is the synchronous two-peer NDN-SVS boundary with RSA-2048 Data signing,
and at that boundary does increasing Fetcher admission width or piggyback
capacity reduce fallback queue residence, Sync receive callback residence,
publication timer misses, and delivery collapse?

### Stage A: RSA rate sweep

Five once-only cells run in ascending order at 200, 400, 600, 800, and 1000
pps/peer with `W10-P4096`. The lowest valid rate with any peer below 98%
attempted/scheduled or aggregate delivery below 98% is the selected boundary.
If none crosses, 1000 pps is selected only as a labeled stress point.

### Stage B: factor intervention

After stage A closes, the runner writes a hash-bound selection record and seals
three cells in fixed order:

| Ordinal | Cell | Fetcher window | Max ApplicationParameters |
|---:|---|---:|---:|
| 1 | `W40-P4096` | 40 | 4096 B |
| 2 | `W10-P7168` | 10 | 7168 B |
| 3 | `W40-P7168` | 40 | 7168 B |

The selected stage-A `W10-P4096` cell is the fourth combination and is never
rerun. With no replication, all contrasts remain descriptive.

### Instrumentation

- Existing exact profile summaries provide Fetcher queue-wait calls/duration,
  Sync receive calls/duration, Mapping/Payload stage counts, and CPU grouping.
- Driver `state-update` events already share one timestamp per update callback;
  the Spec 135 analyzer groups them to recover missing-range burst widths.
- Process-stop events provide exact scheduled/attempted/missed releases.
- Before warmup, the driver creates an RSA-2048 identity, configures
  `security::signingByIdentity(identity)`, signs a probe through the same
  `KeyChainSigner`, and asserts TLV type `SignatureSha256WithRsa`. Identity/key
  generation is outside all measured intervals.
- Peer applications inherit the node-specific environment produced by
  MiniNDN's `popenGetEnv()`: each node has an independent `HOME`, `.ndn/pib.db`,
  private-key directory, client configuration, and working directory. Raw
  `host.popen()` without this environment is an invalid harness path.
- A diagnostic environment value selects Fetcher window 10 or 40 before
  construction. The worktree records the effective value; production defaults
  are untouched.
- A driver option selects 4096 or 7168 bytes for
  `SVSPubSubOptions.maxApplicationParametersSize`.

### Analysis

First report the RSA rate boundary and publication stages, including RSA sign
time. Then compute both window contrasts at fixed piggyback and both piggyback
contrasts at fixed window. Report absolute changes and ratios for delivery,
missed releases, fallback calls, queue wait, Sync receive residence, and
missing-batch tails. Do not compute p-values from one observation per
combination.

## Source Structure

```text
Experiments/
|- build_svs_fetcher_queue_causality.py
|- NDN_SVS_Fetcher_Queue_Causality_Minindn.py
|- analyze_svs_fetcher_queue_causality.py
`- ndn-svs-pubsub-benchmark/svs-fetcher-queue-causality.cpp
tests/python/
`- test_spec135_svs_fetcher_queue_causality.py
specs/135-svs-fetcher-queue-causality/
|- spec.md
|- plan.md
|- research.md
|- tasks.md
|- traceability.md
|- contracts/diagnostic-contract.md
`- evidence/
build/spec135/
`- worktrees/fetcher-queue-causality/
results/spec135-svs-fetcher-queue-causality/
```

## Rollback And Removal

The diagnostic worktree, temporary branch, binaries, and result paths are
isolated. Nothing is installed. A negative result requires no rollback; it is
retained. No code is promoted into NDN-SVS by this Spec.
