# Pre-Implementation Audit: Spec 131

**Date**: 2026-07-21

**Verdict**: PASS

**Scope**: Planning readiness only. No benchmark source, dual build, smoke, or
formal MiniNDN cell has been implemented or executed.

## Gate Results

| Gate | Result | Evidence |
|---|---|---|
| Intent fidelity | PASS | Exactly two pinned subjects, requested five rates, old-first order, pure NDN-SVS boundary |
| Necessity/Occam | PASS | One publisher, one subscriber, one topology, one payload size, two subject modes; no controller or NDNSF layer |
| Source boundary | PASS | Git ancestry identifies `a994401` before `a8a9656`/`15d1bc6`; current public APIs are source-verified |
| Architecture ownership | PASS | Pinned commits and active refs stay immutable; disposable local build branches own the identical Boost 1.71-only patch |
| Measurement validity | PASS | Primary/secondary delay, shared monotonic clock, open-loop pacing, delivery/loss guard, and censoring defined |
| Statistical plan | PASS | One direct observation per subject-rate, descriptive effects only, no confidence interval or p-value claim |
| Failure/retry | PASS | Repairable preflight, sealed 10-cell campaign, one attempt per formal cell, complete rerun under new ID only |
| Security/scope | PASS | NDN-SVS normal public security remains; dependency/process audit excludes NDNSF rather than bypassing it |
| Migration/rollback | PASS | Temporary Boost 1.71 branch commits cannot merge/rebase/push or move active refs; manifests preserve base/head/tree/patch identities before disposable cleanup |
| Requirement/task coverage | PASS | 22/22 FR and 9/9 SC mapped to seven cohesive tasks |

## Code-Aware Evidence

- `a994401:ndn-svs/svspubsub.hpp` contains synchronous `publish()` and no
  `publishAsync()`.
- `a8a9656` adds receive-side parallel processing; `15d1bc6` adds parallel
  production and ordered asynchronous PubSub publication.
- Current `SVSPubSub` exposes `publishAsync()` and public `getSVSync()`;
  `SVSyncCore` exposes receive/production worker configuration and timing/job
  counters.
- Current V2 option resolution matches the old 1 ms Interest lifetime, 500 ms
  suppression, 30 s periodic interval, and 0.1 jitter.
- Both pinned commits currently reject `BOOST_VERSION_NUMBER < 107400` and
  report a 1.74.0 minimum in `wscript`, while the target build environment uses
  Boost 1.71. The build contract therefore permits exactly the same two-line
  `107400`/`1.74.0` to `107100`/`1.71.0` local patch on separate temporary
  branches, requires identical patch hashes and clean post-commit worktrees,
  records base and temporary identities, audits Boost 1.71 linkage, rejects
  Boost 1.74 residue, and prohibits merge/rebase/push/active-ref movement.
- `SVSyncBase` registers the local producer Data prefix, while remote Mapping
  and publication fetches use producer-node-prefixed names. The plan therefore
  includes stable peer-node topology routes in addition to the Sync-group
  route and forbids concrete publication-name/transient-face route injection.
- The existing `examples/run_svs_latency_minindn.sh` launches an NDNSF-built
  example and therefore cannot be the Spec 131 measured binary. Its MiniNDN
  orchestration patterns may be reused only after the new dependency boundary
  is enforced.

## Known, Accepted Design Limits

1. The seven-commit delta includes more than async/thread work. This is
   explicitly modeled as a version-bundle comparison and appears in every
   allowed claim.
2. Old-first block execution confounds subject with time. Matched schedules,
   fixed CPU sets, clean per-cell namespaces, and host telemetry reduce but do
   not eliminate this risk.
3. One observation per subject-rate supports direct descriptive comparison,
   not a variance estimate, confidence interval, or null-hypothesis test.
4. The 256-byte workload does not generalize to segmented publications.
5. The measured binaries are not byte-for-byte builds of the unmodified base
   trees; each is the exact base plus one identical, recorded build-gate patch.
   The patch changes admission of Boost 1.71, not NDN-SVS runtime logic.
6. Live preflight exposes only CPUs 0--3. All roles and both subjects therefore
   share the same frozen four-CPU affinity. Four treatment workers remain
   enabled, so results measure a contended four-core system and cannot be
   generalized to dedicated four-core publisher/subscriber hosts.

None of these limits contradicts the requested question; each blocks only a
stronger interpretation.

## Findings

- Critical: 0
- High: 0
- Medium: 0
- Low: 0

## Implementation Entry Point

Proceed at T001. Formal execution remains forbidden until T004 seals a fresh
campaign after both non-formal subject smokes pass.
