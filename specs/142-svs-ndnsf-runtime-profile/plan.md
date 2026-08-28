# Implementation Plan: NDN-SVS Worker Validation Under the NDNSF Runtime Profile

**Branch**: `142-svs-ndnsf-runtime-profile` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

**Input**: Correct the Spec 140/141 worker benchmark so its NDN-SVS transport
configuration matches the effective current NDNSF runtime profile.

## Summary

Build one exact benchmark binary that retains the two-node bidirectional
publish/subscribe workload and RSA-2048 path, but replaces the forced V2,
one-byte-piggyback stress profile with the effective NDNSF profile: V3, 1 s
Sync Interest lifetime, 1 ms suppression, 30 s periodic Sync, 800-byte
effective piggyback limit, adaptive publication-Fetch window, frozen retry
constants, and equal four-worker Sync processing/production settings. Compare only
`publicationPreparationWorkers=0` against `1`. Qualify a fresh 400 pps pair
before permitting fresh 600/800 pps cells.

## Technical Context

**Language/Version**: C++17 benchmark; Python 3 MiniNDN runner and analyzer

**Primary Dependencies**: current workspace NDN-SVS, ndn-cxx, MiniNDN, NFD,
Boost 1.71, OpenSSL/RSA through ndn-cxx KeyChain

**Storage**: immutable local manifests, JSON/CSV receipts, raw latency samples,
and logs under `results/spec142-svs-ndnsf-runtime-profile/<campaign-id>/`

**Testing**: focused Python runner/analyzer tests; NDN-SVS unit tests against
the exact runtime library; MiniNDN qualification and formal cells

**Target Platform**: Linux MiniNDN host with four logical CPUs, affinity 0--3

**Project Type**: distributed systems performance microbenchmark

**Performance Goals**: accurately offer 400/600/800 pps per peer; measure
throughput and mean/p50/p95/p99 without Fetch-recovery confounding

**Constraints**: two simultaneous publishing/subscribing nodes; 100 Mbps,
10 ms one-way, zero configured loss; RSA-2048; 10/60/10 timing; exactly one
formal run per authorized cell; no configuration tuning after freeze

**Scale/Scope**: one qualification pair plus, conditionally, four higher-rate
cells; no NDNSF runtime feature or wire-protocol modification

## Experiment Design

### Research question

Under the NDN-SVS configuration actually used by current NDNSF
ServiceUser/ServiceProvider, does moving byte-publication preparation from the
Face thread to one FIFO worker reduce delivery latency or extend sustainable
capacity?

### Independent variable

```text
face-inline-rsa: publicationPreparationWorkers = 0
worker-rsa:      publicationPreparationWorkers = 1
```

Every other resolved option, binary, library, workload, topology, timing,
security setting, and CPU allocation is matched.

### Staged matrix

```text
Qualification:
  01 face-inline-rsa @ 400 pps/peer
  02 worker-rsa      @ 400 pps/peer

Authorized only if both qualification receipts are PROFILE_VALID:
  03 face-inline-rsa @ 600 pps/peer
  04 worker-rsa      @ 600 pps/peer
  05 face-inline-rsa @ 800 pps/peer
  06 worker-rsa      @ 800 pps/peer
```

The 400 gate checks identity, resolved configuration, attempted rate, RSA
security counters, raw-data accounting, and zero retry/timeout/Nack activity.
It does not require both modes to sustain full delivery; sustainability is the
measured outcome.

### Causal validity rule

A rate comparison exists only when both mode receipts are `PROFILE_VALID`.
Any retry, timeout, or Nack is retained as a real boundary observation but
disqualifies that pair from a clean publication-worker causal claim. This
ensures the retry budget cannot mask a Fetch failure in a nominally zero-loss
worker experiment.

## Constitution Check

- **Canonical runtime**: PASS. No NDNSF API or wire-protocol change.
- **Security in data path**: PASS. RSA-2048 signing/validation remains enabled
  and is verified in every receipt.
- **CodeGraph/source verified**: PASS. The plan is based on current
  ServiceUser/ServiceProvider option construction and NDN-SVS defaults.
- **Spec-driven durable work**: PASS. Spec, plan, contract, tasks, and audit
  precede implementation.
- **Right verification scope**: PASS. Final evidence is MiniNDN with 60-second
  measured windows.
- **Cohesive tasks**: PASS. Tasks deliver profile freeze, qualification, and
  higher-rate evidence as behavioral units rather than file-by-file steps.
- **Frozen evidence**: PASS. Specs 140/141 remain untouched and are explicitly
  excluded from corrected claims.

## Project Structure

### Documentation

```text
specs/142-svs-ndnsf-runtime-profile/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── traceability.md
├── quickstart.md
├── contracts/
│   └── experiment-contract.md
├── checklists/
│   └── requirements.md
├── evidence/
│   └── pre-implementation-audit.md
└── tasks.md
```

### Implementation and evidence

```text
Experiments/ndn-svs-pubsub-benchmark/
└── svs-rsa-single-worker.cpp
Experiments/
├── NDN_SVS_NDNSF_Profile_Worker_Minindn.py
└── analyze_svs_ndnsf_profile_worker.py
tests/python/
└── test_spec142_svs_ndnsf_profile_worker.py
results/spec142-svs-ndnsf-runtime-profile/<campaign-id>/
```

**Structure Decision**: Reuse the current publication-worker benchmark and
introduce a separate Spec 142 runner/analyzer namespace. Do not edit or import a
frozen Spec 140/141 campaign in a way that could mutate its evidence.

## Implementation Strategy

1. Make the benchmark accept and emit the effective NDNSF profile fields,
   including actual signed publication wire size and Fetch/piggyback counters.
2. Make the runner build one manifest, verify linkage, qualify independent
   pacers at 800 pps, and enforce exact mode parity.
3. Run the 400 pps pair exactly once and evaluate the strict profile gate.
4. Only after that gate passes, run the four 600/800 cells exactly once.
5. Analyze raw samples, freeze receipts and hashes, and make only
   scope-bounded claims.

## Complexity Tracking

No constitution violation or new framework abstraction is required. A new
runner namespace is necessary to prevent the corrected campaign from modifying
or being confused with frozen Specs 140/141.
