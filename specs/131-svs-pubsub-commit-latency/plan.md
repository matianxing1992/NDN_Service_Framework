# Implementation Plan: NDN-SVS PubSub Commit-Latency Comparison

**Branch**: `131-svs-pubsub-commit-latency` | **Date**: 2026-07-21 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/131-svs-pubsub-commit-latency/spec.md`

## Summary

Build one version-compatible, external C++ PubSub benchmark against two
temporary NDN-SVS build branches rooted at the pinned commits and carrying the
same sole Boost 1.71 `wscript` compatibility patch: pre-feature `a994401` using
synchronous `publish()`, and latest `6bb3454` using `publishAsync()` plus
four-worker parallel Sync receive/production. Run pure NDN-SVS
publisher/subscriber processes in a fresh two-host MiniNDN topology at
200/400/600/800/1000 publications/s. Run one cell per subject-rate pair,
execute all 5 old cells first and all 5 latest cells second, and compare
end-to-end PubSub delivery delay without hiding missed release slots, missing
items, or attributing the whole commit delta to one feature. The Face event
loop and high-resolution publication pacer run on independent threads.

## Technical Context

**Language/Version**: C++17 benchmark peer; Python 3 MiniNDN runner, manifest,
and analyzer

**Primary Dependencies**: pinned local NDN-SVS commits, disposable build
branches with the canonical Boost 1.71 gate patch, ndn-cxx, Boost 1.71, NFD,
MiniNDN, Linux network namespaces; no NDNSF runtime dependency

**Storage**: Immutable JSON manifests/summaries, buffered JSONL or CSV event
rows, text process/network evidence under a unique local `results/` campaign

**Testing**: C++ driver self-test, Python contract tests, dual-commit builds
whose only source delta is the identical hashed Boost 1.71 gate patch, two
non-formal 1000 pps MiniNDN admission smokes, then one fresh 10-cell formal
campaign

**Target Platform**: One Linux MiniNDN host restricted to logical CPUs 0--3,
with a common monotonic kernel clock across namespaces

**Project Type**: Cross-repository performance experiment harness

**Performance Goals**: Characterize PubSub delivery at exactly
200/400/600/800/1000 publications/s; do not prerequire an improvement

**Constraints**: Old block before latest block; 60-second measured window;
small fixed payload; V2 wire compatibility; no loss injection; no NDNSF; no
selective formal reruns; no mutation or ref movement of pinned NDN-SVS commits;
only identical unmerged local Boost 1.71 build commits are allowed

**Scale/Scope**: 2 subjects x 5 rates x 1 observation = 10 formal cells; each
cell includes 10 s warmup + 60 s measurement + 10 s drain, plus bounded setup

## Constitution Check

*GATE: Passed before design and rechecked after design.*

- **Canonical runtime**: Not applicable to the measured path. This Spec does
  not invoke NDNSF and does not change its runtime API.
- **Security path**: No NDNSF security path is bypassed because it is outside
  scope. Each NDN-SVS version retains its normal public signing/validation
  behavior; the manifest records that signing code is an intervening-version
  confound.
- **CodeGraph/source verified**: CodeGraph confirmed current `publishAsync()`,
  `getSVSync()`, `setParallelSyncProcessing()`, and
  `setParallelSyncProduction()` sources. Git history confirms `a994401` is the
  parent-side boundary before `a8a9656` receive parallelization and `15d1bc6`
  async/parallel production.
- **Spec-driven durable work**: Spec, design, contracts, tasks, traceability,
  and audit own the benchmark before implementation.
- **Right validation scope**: Final evidence is MiniNDN, not host NFD. Every
  measured cell has a 60-second window.
- **Cohesive tasks**: Tasks close version builds, measurement semantics,
  campaign authority, old execution, new execution, and comparison; tests,
  implementation, execution, and evidence remain together where they prove one
  outcome.
- **GSD continuity**: Local GSD health is valid. Spec 131 records a resumable
  manifest and phase boundary without replacing the unrelated active DI phase
  in `.planning/STATE.md`.
- **ARS experiment gate**: Research question, subjects, outcomes, controls,
  confounders, sample count, censoring, analysis, and interpretation limits are
  preregistered in [research.md](research.md) and the measurement contract.

Post-design recheck: PASS. No constitution exception is required.

## Source-Verified Version Boundary

```text
a994401  svspubsub: piggyback bounded mappings and publication Data
    |
    +-- a8a9656  parallel receive processing + local Sync batching API
    +-- 15d1bc6  parallel production + ordered async PubSub publishing
    +-- 64a4476  V2 signed-Interest implementation change
    +-- 6222a4a  V3 synchronization protocol
    +-- 64b6ff7  sparse mapping recovery
    +-- bc75baf  failure-atomic segmented publication
    +-- 6bb3454  bounded segmented fetch and repair recovery  [latest]
```

The formal comparison uses only the first and last nodes. The intervening list
is stored in the candidate manifest and repeated in the final report. It is a
version-bundle evaluation. A component-isolation study would need additional
subjects and is outside Spec 131.

## Experimental Design

### Research Question

On the same zero-loss two-host MiniNDN link, how does the latest NDN-SVS
async/parallel PubSub configuration change application-visible PubSub delivery
delay and sustainable delivery relative to the final pre-feature sync/serial
commit as the offered rate rises from 200 to 1000 publications/s?

### Subjects and Deliberate Difference

| Control | `baseline-sync-serial` | `latest-async-parallel` |
|---|---|---|
| Git commit | `a994401...` | `6bb3454...` |
| Publish API | `publish()` | `publishAsync()` |
| Sync receive workers | unavailable/serial | enabled, 4 workers, queue 4096 |
| Sync production workers | unavailable/serial | enabled, 4 workers, queue 4096, worker signing/extra-block construction enabled |
| Sync batching | unavailable | explicitly disabled |
| Wire profile | historical V2 | explicitly V2 |
| V2 timers | 1 ms / 500 ms / 30 s / 0.1 jitter | explicitly matched |

Both peers in a treatment cell enable receive and production worker pools. The
publisher uses the async API; the subscriber exercises parallel receive. The
driver records the resolved latest settings before publishing.

### Fixed Controls

- topology: publisher--subscriber, 100 Mbps, 10 ms one-way delay, 0% loss;
- processes: one NFD per namespace and one benchmark process per role;
- forwarding: multicast strategy plus explicit neighbor routes for the unique
  Sync group and stable peer node prefixes; PubSub registers local producer
  filters, and no concrete publication-name or transient-face route is added;
- publication: 256 deterministic bytes, unique logical ID and name, no
  segmentation, fixed freshness/options;
- timing: subscriber-first startup, 5 s convergence, 10 s warmup, 60 s
  measurement, 10 s drain;
- pacing: an independent high-resolution pacer thread uses absolute monotonic
  deadlines and calls a thread-safe `io_context::post` publication adapter;
  the Face event loop runs on its own thread and executes the pinned
  `publish()`/`publishAsync()` implementation; no Face scheduler callback
  generates offered load and there is no unbounded catch-up loop; slots more
  than two periods late are skipped and all actual wake times are recorded;
  the pacer is pinned
  to CPU 0 with requested `SCHED_FIFO` priority 1 and a 50 us final spin, while
  the Face thread is pinned to CPU 1, with all pin/priority outcomes recorded;
- CPU: publisher, subscriber, NFD, and orchestration share the same fixed
  four-CPU affinity set `0-3` in every cell; treatment worker count remains 4,
  and the resulting contention is reported rather than hidden;
- ordering: 5 baseline cells first, then 5 treatment cells, in ascending rate
  order so each treatment cell has one direct baseline match;
- logging: buffered hot-path events, identical log level and capture settings;
- host: same machine, no concurrent formal campaign, source/build identities
  unchanged throughout.
- build environment: same compiler, ndn-cxx, Boost 1.71 headers/libraries, and
  byte-identical `wscript` build-only patch for both subjects; no Boost 1.74
  runtime linkage.

### Formal Matrix

| Block | Subject | Rates (publications/s) | Repetitions | Cells |
|---:|---|---|---:|---:|
| 1 | baseline-sync-serial | 200, 400, 600, 800, 1000 | 1 | 5 |
| 2 | latest-async-parallel | 200, 400, 600, 800, 1000 | 1 | 5 |
| **Total** | | | | **10** |

Each block uses the fixed ascending rate order. This preserves the requested
old-first execution and yields a direct old/latest observation at every rate.

### Timing Semantics

Each payload carries a logical item ID and the absolute release deadline. The
pacer records `scheduledNs` and `attemptedNs`; the Face-side API task records
`apiEnterNs`, `apiReturnNs`, and returned SVS sequence. `scheduled` counts
release opportunities, `attempted` counts adapter admissions, and `api-return`
counts completed pinned API calls. The subscriber records:

1. `stateUpdateNs`: first update callback whose producer range covers the SVS
   sequence, if observed; and
2. `deliveryNs`: first `SubscriptionCallback` for the logical item.

Primary application-visible delay is `deliveryNs - scheduledNs`. Secondary
state synchronization delay is `stateUpdateNs - scheduledNs`. API duration is
`apiReturnNs - apiEnterNs`. This intentionally includes treatment queue/defer
time in the user-visible primary endpoint.

### Outcomes and Interpretation

Per cell:

- scheduled release slots, attempted publications, achieved attempted rate,
  and missed release slots;
- emitted, first-delivered, missing, duplicate, and out-of-order totals;
- attempted/scheduled offered-load fulfillment and delivered/attempted ratio;
- delivered-only p50/p95/p99/max PubSub delay;
- state-update p50/p95/p99 where observable;
- deadline-capped p95/p99 with undelivered items assigned the drain deadline;
- publish API p50/p95/p99;
- process CPU/RSS and NFD/link packet counters;
- treatment worker submitted/completed/dropped/stale/queue/timing counters.

A cell is sustained-rate valid only if attempted rate is within ±2% and its
process/evidence checks pass. Both subjects MUST pass a non-formal 1000 pps
admission smoke before the manifest is sealed. Delivered-only latency is never
interpreted without attempted/scheduled and delivered/attempted. Because there
is one observation per subject-rate pair, per-rate differences are descriptive
direct effects only: there is no bootstrap interval, p-value, or population
claim. Labels describe only the two pinned version bundles in this run.

## Runtime and Evidence Architecture

```text
NDNSF repository (experiment ownership only)
├── dual-commit worktree/build controller
├── external version-compatible C++ PubSub benchmark
├── MiniNDN cell/campaign runner
├── manifest + analyzer + contract tests
└── Spec 131 design, audit, and summaries

NDN-SVS temporary local build branches (immutable bases + identical patch)
├── a994401 + Boost-1.71 gate patch -> baseline build/libndn-svs.so
└── 6bb3454 + Boost-1.71 gate patch -> treatment build/libndn-svs.so

MiniNDN cell
publisher namespace: NFD + matching benchmark binary
        100 Mbps / 10 ms / 0% loss
subscriber namespace: NFD + matching benchmark binary
```

The external benchmark uses conditional API compatibility at compile time.
Each NDN-SVS temporary build branch additionally carries the same canonical
two-line `wscript` patch that lowers only the configure-time Boost guard and
diagnostic from 1.74 to 1.71. Base and temporary identities, patch bytes/hash,
and clean post-commit status are recorded. `ldd`, compile/link commands, binary
hashes, and process command lines prove Boost 1.71 linkage and absence of Boost
1.74 or NDNSF runtime dependencies.

## Project Structure

### Documentation

```text
specs/131-svs-pubsub-commit-latency/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/measurement-contract.md
├── quickstart.md
├── tasks.md
├── traceability.md
├── checklists/requirements.md
├── checklists/pre-implementation-audit.md
└── evidence/
```

### Planned Source and Test Surfaces

```text
Experiments/ndn-svs-pubsub-benchmark/
├── svs-pubsub-bench.cpp
└── README.md

Experiments/
├── build_svs_pubsub_commit_bench.py
├── NDN_SVS_PubSub_Commit_Latency_Minindn.py
└── analyze_svs_pubsub_commit_latency.py

tests/python/
└── test_spec131_svs_pubsub_commit_latency.py

build/spec131/                       # ignored, disposable worktrees/builds
results/spec131-svs-pubsub-commit-latency/<campaign-id>/
```

**Structure Decision**: Keep durable benchmark orchestration external to both
pinned subjects so neither source commit is modified. Repository location does
not imply NDNSF runtime use; dependency and process audits enforce the boundary.

## Failure, Retry, and Recovery Rules

- Preflight/smoke is explicitly non-formal and may be repaired before campaign
  creation.
- The formal manifest is content-addressed before the first baseline cell.
- One campaign process owns a lock and one unique output directory.
- Each cell has one attempt. There is no automatic retry, output overwrite, or
  selective replacement.
- An incomplete baseline block stops treatment admission.
- A valid but negative performance cell remains evidence.
- Repair after a formal failure requires a new campaign ID and a complete fresh
  10-cell matrix. The prior campaign remains immutable.
- A base/tree, temporary branch/head/tree, Boost patch, driver, analyzer,
  build, linkage, or manifest hash change invalidates continuation under the
  same campaign ID.

## Expected Runtime and Capacity

The 10 measured cells contain 13 minutes 20 seconds of warmup/measurement/drain
time alone. With namespace startup, convergence, capture, and cleanup, budget
approximately 20--30 minutes plus two patch-audited builds. The measured
windows schedule 180,000 publications per subject across five rates, 360,000
total; buffered compact event rows are mandatory.

## Complexity Tracking

No constitution violation is accepted. Compatibility is limited to the
external driver's public-API adapter and identical temporary `wscript`
Boost-1.71 build-gate commits. Neither changes NDN-SVS runtime semantics, and
both are hash-bound with explicit deletion/non-merge rules.
