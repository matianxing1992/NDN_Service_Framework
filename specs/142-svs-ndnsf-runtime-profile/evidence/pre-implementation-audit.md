# Pre-Implementation Audit: Spec 142

**Date**: 2026-07-23

**Verdict**: PASS — planning is implementation-ready; no formal cell is
authorized until T001/T002 gates pass.

## Intent Fidelity

PASS.

- The protocol is V3.
- The two-node bidirectional publish/subscribe workload remains unchanged.
- The benchmark uses the effective NDNSF NDN-SVS transport profile instead of
  the Spec 140/141 forced-Fetch profile.
- Retry budgets remain fixed runtime constants, but any retry, timeout, or Nack
  invalidates a clean zero-loss worker comparison.
- Specs 140/141 remain frozen and are explicitly excluded from corrected
  performance claims.

## Source And Configuration Evidence

| Claim | Current source evidence | Audit result |
|---|---|---|
| NDNSF defaults to V3 | `ndn-service-framework/ServiceUser.cpp:37` and `ServiceProvider.cpp:37` | PASS |
| V3 resolves to 1 s Interest lifetime and 30 s periodic Sync | `/home/tianxing/NDN/ndn-svs/ndn-svs/sync-protocol.cpp:22-40` | PASS |
| NDNSF runner sets 1 ms suppression | `Experiments/NDNSF_NewAPI_Minindn_Perf.py:1688` | PASS |
| Effective piggyback limit is 800 bytes | `/home/tianxing/NDN/ndn-svs/ndn-svs/svspubsub.hpp:74`; ServiceUser/Provider do not override the option | PASS |
| Runner's 4096 environment value is not effective | `Experiments/NDNSF_NewAPI_Minindn_Perf.py:1695-1696`; no consumer exists in current ServiceUser/Provider | PASS |
| NDNSF uses adaptive Fetch window | `ServiceUser.cpp:357-370,937-952` and corresponding Provider code | PASS |
| Parallel Sync defaults are four workers/queue 256 | `ServiceUser.cpp:985-1018` and corresponding Provider code | PASS |
| Specs 140/141 forced the wrong profile | `svs-rsa-single-worker.cpp:691-709` selects V2, one-byte piggyback, window 64, and 5 ms batching | CONFIRMED |

The contract freezes the source-resolved values. Exported environment values
alone are not accepted as evidence.

## Occam And Ownership

PASS.

- The full NDNSF application workflow is not added to this microbenchmark.
- The sole treatment remains the existing NDN-SVS
  `publicationPreparationWorkers` option.
- No ServiceUser, ServiceProvider, NDNSF wire protocol, retry algorithm,
  periodic timer, or security behavior change is authorized.
- A separate Spec 142 runner/result namespace is necessary to prevent corrected
  evidence from being confused with frozen Specs 140/141.

## Security, Failure, And Resource Boundaries

PASS.

- RSA-2048 sign/validate counts and failures are receipt gates.
- Exact binary and runtime NDN-SVS/ndn-cxx library hashes prevent the previously
  observed installed/workspace linkage ambiguity.
- The four-logical-CPU affinity and all active Sync/publication worker pools are
  reported; no claim assumes additional cores.
- Overload-induced timeout/retry is preserved, not tuned away, but is correctly
  classified as a different failure regime.
- Started formal cells are once-only and immutable.

## Experiment Design Review

PASS for descriptive evidence.

- Treatment: Face-inline publication preparation versus one FIFO worker.
- Controls: binary, libraries, V3, timers, piggyback, Fetch, Sync worker pools,
  topology, traffic, payload, RSA, CPU affinity, timing, and pacers.
- Qualification: fresh 400 pps pair before 600/800 authorization.
- Metrics: attempted/delivered throughput, ratio, raw
  mean/p50/p95/p99, wire size, piggyback, Mapping/Publication Fetch,
  retry/timeout/Nack, RSA, CPU, thread, and queue counters.
- Statistical boundary: one run per mode/rate is descriptive only and does not
  estimate variance or support population-level significance claims.

## Cross-Artifact Coverage

| Requirement group | Covered by |
|---|---|
| Frozen evidence and claim boundary | T005 |
| Build/link/profile identity | T001 |
| V3, piggyback, Fetch, Sync, RSA, CPU controls | T001, T002 |
| Two-peer workload, pacing, timing, receipts | T002, T003 |
| Qualification and conditional matrix | T003, T004 |
| Raw metrics, classification, final claims | T005 |

All 19 functional requirements and five measurable outcomes map to at least one
cohesive task. No task is unowned, duplicated, malformed, or mechanically split.

## Tool Gates

- Spec Kit prerequisite check: PASS.
- Deterministic strict structure audit: PASS with zero blockers and warnings.
- GSD health validation: PASS; one unrelated historical plan lacks a summary.
- Agent-context extension: the configured script path is absent from this
  checkout. The managed `AGENTS.md` Spec Kit pointer was updated manually to
  Spec 142 and this fallback is recorded here.

## Readiness Decision

Implementation may begin at T001. MiniNDN execution remains blocked until:

1. the exact build/runtime manifest passes;
2. both independent pacers pass +/-2% at 800 pps;
3. the runner's classification tests pass; and
4. the fresh 400 pps qualification stage is explicitly started.
