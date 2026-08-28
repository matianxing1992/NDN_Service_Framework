# Implementation Plan: NDN-SVS Zero-Loss Fetch Timeout Causality

**Branch**: `143-svs-zero-loss-fetch-causality` | **Date**: 2026-07-24 |
**Spec**: [spec.md](spec.md)

**Input**: Diagnose the publication and Mapping Fetch timeout/retry boundary
preserved by the frozen Spec 142 400 pps qualification.

## Summary

Add diagnostic-only structured NDN logs to the existing NDN-SVS Fetcher,
SVSyncBase publication producer lookup path, MappingProvider query path, and
SVSPubSub fallback state machine. Extend the existing RSA worker benchmark with
measurement-window process-resource snapshots. Build a separate Spec 143
binary and run one exactly-once 400 pps worker MiniNDN cell under the unchanged
Spec 142 NDNSF V3 profile. Correlate both peer logs by full Interest name,
Nonce, and consumer attempt ID, classify each timeout, and close with either at
least 95% coverage or an explicit inconclusive boundary.

## Technical Context

**Language/Version**: C++17 NDN-SVS/benchmark; Python 3 runner, analyzer, tests

**Primary Dependencies**: current workspace NDN-SVS, ndn-cxx, MiniNDN, NFD,
Boost 1.71, OpenSSL/RSA through ndn-cxx KeyChain

**Storage**: immutable JSON/CSV/log artifacts under
`results/spec143-svs-zero-loss-fetch-causality/<campaign-id>/`

**Testing**: NDN-SVS focused/unit suite; Python classifier/runner tests;
one staged MiniNDN diagnostic cell

**Target Platform**: Linux MiniNDN host with four logical CPUs, affinity 0--3

**Project Type**: distributed-systems causal diagnostic

**Performance Goals**: preserve 400 pps per-peer attempted load within +/-2%;
classify at least 95% of measurement-window timeouts

**Constraints**: Spec 142 immutable; 10/60/10 timing; no policy tuning; no
production fix; no cross-peer monotonic-clock subtraction; no automatic rerun

**Scale/Scope**: one mandatory worker cell; at most one conditionally authorized
inline cell; four NDN-SVS logging seams; one process-resource snapshot path

## Experiment Design

### Research question

When NDN-SVS reports publication or Mapping Fetch timeouts on a MiniNDN link
configured with zero loss, at which observable boundary does the response path
fail: producer observation/store lookup, producer response, consumer arrival,
or validation?

### Hypotheses and discriminating evidence

| ID | Hypothesis | Prediction | Falsifier |
|---|---|---|---|
| H1 | Producer DataStore miss/name mismatch | Producer observes Interest and logs store miss; no put | Matching hit and put |
| H2 | A response only helps a later retry | Producer hit/put matches the timed-out Nonce; a later same-name retry receives Data | No matching producer put or no later Data |
| H3 | Producer puts Data but consumer never receives it | Hit and put exist; no consumer Data callback by drain end | Consumer Data callback exists |
| H4 | Interest never reaches producer/NFD aggregates it | Consumer dispatch/timeout has no producer observation | Matching producer observation |
| H5 | Validation creates the Fetch timeout | Consumer Data callback/validation begins before same-attempt timeout | Fetcher source ordering or trace shows timeout before Data |

H5 is expected to be falsified by the current Fetcher implementation because it
removes the pending Interest on Data before starting validation. Validation
outcomes remain independent trace fields, not primary timeout classes.

### Controlled configuration

The complete configuration is in
[contracts/diagnostic-contract.md](contracts/diagnostic-contract.md). The sole
new treatment is diagnostic observability. Worker mode, V3, 800-byte piggyback,
Fetch policies, RSA, topology, payload, workload, affinity, and timing remain
identical to Spec 142.

### Staged execution

```text
Stage A: unit and synthetic-trace gates
Stage B: exactly one worker-rsa @ 400 pps/peer, 10/60/10
Stage C: no run by default
         one inline-rsa @ 400 only if Stage B has zero timeouts or the
         classifier establishes a mode-dependent ambiguity
```

The Stage B result is retained whether it reproduces, fails, or is
inconclusive. Stage C has no automatic path.

### Analysis

The primary endpoint is timeout classification coverage, not latency or
delivery improvement. Counts and same-clock durations are descriptive. Two
peers are paired participants in one network cell, not independent replicates.
No p-values, confidence intervals, or population claims are made from n=1.

## Constitution Check

- **Canonical runtime**: PASS. The existing RSA benchmark and current NDN-SVS
  library remain the runtime subjects.
- **CodeGraph/source verified**: PASS. Fetcher timeout occurs before
  validation; SVSyncBase silently returns on DataStore miss; SVSPubSub owns
  outer retry/backoff.
- **Spec-driven durable work**: PASS. Spec, contract, tasks, and audit precede
  implementation.
- **Right verification scope**: PASS. Final evidence is a 60-second MiniNDN
  measurement window.
- **Security**: PASS. RSA sign/validate remains enabled and unchanged.
- **Evidence preservation**: PASS. Spec 142 is read-only baseline evidence.
- **Cohesive tasks**: PASS. Tasks are organized as observability, causal
  analyzer/runner, and once-only evidence closure.

## Project Structure

### Documentation

```text
specs/143-svs-zero-loss-fetch-causality/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── traceability.md
├── quickstart.md
├── contracts/diagnostic-contract.md
├── checklists/requirements.md
├── evidence/pre-implementation-audit.md
└── tasks.md
```

### Implementation and evidence

```text
/home/tianxing/NDN/ndn-svs/ndn-svs/
├── fetcher.cpp
├── mapping-provider.cpp
├── svsync-base.cpp
└── svspubsub.cpp

Experiments/ndn-svs-pubsub-benchmark/
└── svs-rsa-single-worker.cpp
Experiments/
├── build_svs_zero_loss_fetch_causality.py
├── NDN_SVS_Zero_Loss_Fetch_Causality_Minindn.py
└── analyze_svs_zero_loss_fetch_causality.py
tests/python/
└── test_spec143_svs_zero_loss_fetch_causality.py
results/spec143-svs-zero-loss-fetch-causality/<campaign-id>/
```

**Structure Decision**: Keep diagnostic logging at the NDN-SVS boundaries that
own the events, but keep experiment orchestration/classification in NDNSF's
Spec 143 namespace. No diagnostic API is added to NDN-SVS.

## Implementation Strategy

1. Emit structured logs without locks, state transitions, packet mutation, or
   callbacks beyond existing paths.
2. Add process-resource snapshots to the existing benchmark schema through a
   new Spec 143 summary version; preserve prior schemas.
3. Build an analyzer against synthetic traces first and require causal
   classification/clock-domain tests to pass.
4. Freeze one binary/library manifest and start the worker cell once.
5. Generate a measured report and a fallacy scan. Do not tune or fix the
   mechanism in this Spec.

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-24
- Verification Status: MEASURED_AND_AUDITED
- Version Label: code_result_v1

## Complexity Tracking

No constitution violation is required. Four source logging seams are necessary
because publication and Mapping requests have separate producer paths and no
one component observes consumer dispatch, producer lookup, and consumer
callback together.
