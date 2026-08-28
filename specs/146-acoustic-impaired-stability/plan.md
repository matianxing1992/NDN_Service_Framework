# Implementation Plan: Acoustic Loss/Reorder Stability

**Spec**: [spec.md](spec.md)  
**Date**: 2026-07-24

## Summary

Repair the generic scheduling, retry, and FEC ownership defects exposed by
frozen Spec 144. The minimal design prevents recovery while signed Data owns a
cursor, allows already authenticated repair to complete a source after its
exact-name attempt expires, preserves finite retry when repair is insufficient,
replaces the all-history repair scan with direct group indexing, retires
finished group state, and verifies the unchanged acoustic contract in a fresh
16-cell MiniNDN campaign.

No public API is changed. Spec 147 will separately design the convenience
surface after this feature closes.

## Technical Context

- C++17 Core, ndn-cxx, Boost.Test
- Python 3 runner/analyzer tests
- two-node MiniNDN/NFD with the frozen Spec 144 netem profiles
- environment-safe build parallelism: `-j2`
- 60-second measured window, one invocation per formal cell

## Root-Cause Model

1. `tryRecover()` regarded every absent `m_signedOpaque` entry as missing,
   even when its cursor remained in `m_payloadInFlight` or
   `m_payloadProcessing`. Reordered repair Data therefore caused duplicate
   GF(256) work and made the later signed source appear late.
2. Completed out-of-order groups still consumed the bounded unresolved-group
   scheduling horizon. One missing early cursor could therefore stop new
   source scheduling after the first eight groups.
3. Mapping discovery was anchored only to the contiguous next cursor. The same
   early gap froze discovery despite authenticated higher-cursor completion.
4. Payload retry lifetime removed the stream-period margin from the existing
   missing-timeout decision, turning valid reordered responses on the frozen
   20 +/- 10 ms path into false timeouts.
5. FEC repair could not be consumed until all exact-name retries exhausted,
   even after an Interest timeout had ended network ownership and sufficient
   authenticated repair was already present.
6. Every admitted source scanned all retained `m_repairs` groups to rediscover
   its group, and completed group state lacked a full retirement path.
7. The atomic scheduler admitted each mapped group as `sources + repairs`
   before considering the next authenticated group. Under backlog, optional
   repair therefore consumed packet and validation capacity ahead of later
   application sources.
8. A repair timeout or Nack continued through the ordinary retry path even
   after every protected source had completed, retaining a cursor and
   generating traffic that could no longer contribute to recovery.
9. Every newly verified Mapping block copied the complete retained block map
   and ran the all-block `rebuild()` path once or twice. Acoustic uses one
   six-entry Mapping block per 40 ms group, so this work grew with the retained
   window and saturated the Face thread even though crypto workers were idle.
10. The inherited runner enabled the high-rate Stream packet timeline probe
    at sample rate one. Core already documents that this diagnostic can become
    part of the latency being measured.

Each hypothesis is tested independently before broader MiniNDN validation.

## Design

### Exclusive ownership and bounded early recovery

Keep cursor ownership mutually exclusive:

```text
scheduled -> in-flight -> processing -> signed completion
          \-> timeout eligible -> authenticated FEC recovery -> recovered completion
                              \-> exact-name retry -> in-flight
                              \-> retry exhaustion -> terminal missing
```

`tryRecover()` rejects every absent source that has not reached timeout
eligibility or terminal missing, and every cursor still owned by network,
validation, or recovery processing. Timeout eligibility never changes the FEC
capacity, prefetch window, or retry budget: sufficient authenticated repair
may finish the cursor immediately; otherwise the existing retry path resumes.

### Direct group index and bounded retirement

On first authenticated group observation, record the generic relation from
each source cursor to its group ID. Source admission looks up one group rather
than scanning all repair groups. Once all source cursors in a group are
complete and no recoverable cursor remains, erase its repairs, exhausted flag,
and index entries. Retention cleanup also removes state older than the
authenticated recovery horizon.

### Unresolved-horizon accounting

Do not create a new policy or workload knob. Preserve whole-group mapping, the
existing source/repair wire contract, and the current finite retry budget. A
group whose every cursor is already complete consumes neither an Interest slot
nor application-processing capacity and therefore is skipped when filling the
bounded unresolved-group horizon. The actual concurrent Interest limit remains
unchanged.

Within one scheduling pass, inspect the bounded authenticated Mapping horizon,
admit every eligible source first, then spend only the remaining packet budget
on repair symbols. The packet/processing limits remain authoritative; the
64-group scan guard is only a CPU bound and is not a second interpretation of
`decision.window`. If a repair attempt reaches timeout/Nack after all sources
are complete, retire that cursor without a retry.

### Incremental verified Mapping admission

Keep the existing atomic rebuild for a fork, gap, out-of-order arrival, or
quarantined block. For the ordinary strict successor of the signed checkpoint,
validate the previous digest, first cursor, immutable-name reservations, and
Mapping-v2 group continuation, then append its bindings and evict only the
oldest locally retained block. This changes the common path from rebuilding
the full retained Mapping window per sample to work proportional to the new
block while preserving the same security decisions and bounds.

The formal runner explicitly sets
`NDNSF_STREAM_PACKET_TIMELINE_TRACE=0`. An opt-in Nack diagnostic may enable
the probe with an explicit sample rate. This is measurement hygiene, not a
Stream policy or prefetch change.

### Rejected alternatives preserved as diagnostics

- Expanding the window from authenticated `latestProduced` increased the
  source backlog and was reverted.
- Fetching repair only after a source timeout reduced repair traffic but
  collapsed recovery success and raised combined p95 into seconds.
- Fetching one of two repairs proactively reduced protection strength and
  worsened combined latency/recovery.
- Retiring an in-flight repair before its Data/Nack/timeout outcome converted
  usable repair into late/nonproductive traffic and was reverted.

These negative diagnostics constrain the fix: preserve two-repair protection
and network/validation ownership, while correcting ordering and terminal
lifecycle only.

### Evidence correction

The new analyzer reports counts with matching units:

```text
sourceRecoveryRatio = recoveredSourceCount / recoveryEligibleSourceCount
groupRecoveryRatio  = recoveredGroupCount / recoverableGroupCount
```

`recoveryAttempts` remains a work counter and is never used as the denominator
for recovered sources.

## Verification Layers

1. focused deterministic Boost tests for repair-before-source reorder,
   processing ownership, one/two/over-capacity missing sources, exact-once
   delivery, bounded group retirement, no all-history scan behavior, and
   source-before-repair ordering across an atomically connected Mapping
   backlog;
2. full native unit tests and forced Python binding rebuild/tests;
3. one non-formal zero/loss/reorder/combined diagnostic sequence;
4. preflight and immutable subject/command/analyzer freeze;
5. one fresh 16-cell formal MiniNDN matrix;
6. post-implementation CodeGraph blast-radius and neutrality audit.

## Experiment Design

### Research question

Does correcting generic recovery ownership and lifecycle make the existing
25-block/s variable-size two-repair stream stable under the frozen Spec 144
loss and reorder profiles without workload-specific logic?

### Variables

- Independent: profile (`zero-loss`, `loss`, `reorder`, `combined`).
- Fixed: binary, topology, cadence, source extent cycle, payload sizes, FEC,
  interest limits, thresholds, measurement window, runner/analyzer.
- Dependent: delivery, latency distribution, gap, future/mapping utility,
  retries/timeouts/Nacks/late arrivals, recovery and Interest conservation.

### Repetitions and decision rule

Run 1/5/5/5 cells respectively. Zero-loss requires 1/1; each impaired
treatment requires at least 4/5. Preserve every terminal or incomplete cell.
No automatic or selective rerun is allowed.

### Threats and controls

- Scheduler/network nondeterminism: five repetitions and exact intervals.
- qdisc misapplication: record effective ingress/egress child qdiscs before
  and after each cell.
- source drift: freeze hashes before the first formal cell.
- post-hoc tuning: freeze thresholds and analyzer; reject any mutation.
- historical contamination: new Spec 146 runner/output names and hashes.

## Anticipated Changed Files

```text
ndn-service-framework/Stream.hpp
ndn-service-framework/Stream.cpp
ndn-service-framework/common.hpp
ndn-service-framework/ServiceUser.cpp
ndn-service-framework/ServiceProvider.cpp
tests/unit-tests/stream.t.cpp
tests/unit-tests/encrypted-permission-response.t.cpp
pythonWrapper/src/ndnsf/_ndnsf.cpp             # only if generic status grows
pythonWrapper/ndnsf/streaming.py                # only for status parity
Experiments/NDNSF_Acoustic_Stability_Minindn.py
Experiments/run_spec146_acoustic_stability_matrix.py
Experiments/analyze_spec146_acoustic_stability.py
tests/python/test_spec146_acoustic_stability.py
specs/146-acoustic-impaired-stability/**
```

## Constitution Check

- CodeGraph first: PASS.
- Spec Kit durable artifacts: PASS.
- GSD resumability and one-owner evidence: PASS.
- ARS experiment contract: PASS.
- Security-preserving exact-name/signature path: PASS with an explicit open
  predecessor confidentiality finding; no new confidentiality claim.
- Scope/Occam: PASS. The design removes invalid work and global scans; it does
  not add a new prefetch policy or app semantic branch.
