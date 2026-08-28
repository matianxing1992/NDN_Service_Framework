# Research and Experiment Decisions

## Material Passport

- **Origin Skill**: experiment-agent
- **Origin Mode**: plan
- **Origin Date**: 2026-07-22
- **Verification Status**: SOURCE-VERIFIED DESIGN; NOT YET EXECUTED
- **Version Label**: spec133_code_plan_v1

## Experiment Overview

- **Title**: Synchronous NDN-SVS PubSub Stage Profiling
- **Research question**: Which synchronous NDN-SVS publisher, Sync, Mapping,
  fetch, validation, decode, or delivery stage first becomes the dominant
  service-demand or waiting bottleneck as bidirectional PubSub load increases
  from 200 to 1000 publications/s per peer?
- **Hypothesis**: The limiting stage may shift with rate. Candidate mechanisms
  include repeated Mapping candidate encoding/piggyback packing, cryptographic
  signing, serial Face/io_context Sync processing, and fallback Mapping/Payload
  fetching.
  No candidate is accepted without the formal stage/rate evidence.
- **Type**: Generic systems benchmark with descriptive within-cell profiling

## Variables

- **Independent variable**: Per-peer target rate: 200, 400, 600, 800, 1000.
- **Dependent variables**: Per-stage service duration/distribution/count,
  thread-specific exclusive demand share, scheduler/queue/external wait,
  attempted rate, delivered rate, delivery ratio, and path frequency.
- **Controls**: Exact subject commit and patches, signing profile, topology,
  payload, naming, timers, CPUs, peer roles, warmup/window/drain, logging sample
  rule, execution order, and no-retry policy.
- **Known confounds**: Instrumentation overhead, one observation per rate,
  temporal drift from ascending execution, OS/NFD scheduling, shared-host CPU
  contention, historical shared-container synchronization behavior, and branch-
  frequency changes induced by overload.

## Decision 1: Use the last pre-feature commit only

**Decision**: Profile base commit `a9944019f76791773604999f00128057b9534ace`
plus the canonical Boost 1.71 patch and a frozen profiling-only patch.

**Rationale**: Git ancestry and historical header/source inspection show this
commit precedes the receive-parallelization and ordered async/parallel
production commits. It exposes synchronous `publish()` but no `publishAsync()`
or worker APIs. README and the public header do not document a threading
contract; only the examples claim cross-thread safety.

**Alternative rejected**: Disabling async/workers in the latest source would
not be the requested version without those features; later correctness and
protocol changes would remain in the subject.

## Decision 2: Profile the actual historical call graph

**Decision**: Instrument exact historical functions rather than importing the
latest source's statistics API.

**Source evidence**:

- `SVSPubSub::publish` constructs and signs inner Data, encodes it, queues
  piggyback data, and calls `publishPacket`.
- `SVSyncBase::publishData` builds/signs/stores outer Data, updates local state,
  and calls `Face::put`.
- `SVSPubSub::insertMapping` creates a timestamp, queues notification Mapping,
  and inserts into `MappingProvider`.
- `SVSyncCore::sendSyncInterest` obtains extra Mapping/piggyback Data, encodes
  the version vector and ApplicationParameters, signs, and expresses a Sync
  Interest.
- The peer decodes Mapping/piggyback blocks, merges the vector, processes
  subscriptions, and either delivers from piggyback or enters Mapping/Payload
  fetch paths.
- `MappingProvider::onMappingQuery` performs range lookup, MappingList encode,
  Mapping Data build/sign/put; `fetchNameMapping` validates and decodes it.
- `Fetcher` exposes queue, express, receive, validation, Nack, timeout, and
  retry boundaries; `MemoryDataStore` exposes insert/find boundaries.

## Decision 3: Measure CPU, aggregate, lock, and wait spans separately

**Decision**: Every stage is classified as leaf CPU, aggregate, lock wait,
queue wait, external wait, or milestone.

**Rationale**: Adding a top-level `publish()` duration to its signing/encoding
children double-counts work. Treating Interest RTT as CPU time misidentifies
network/NFD delay as a library computation bottleneck.

**Alternative rejected**: Ranking raw p99 values regardless of span kind.

## Decision 4: Combine exact counters with sampled spans

**Decision**: Time/count every operation in fixed-size aggregate accumulators;
emit detailed spans for all stages of deterministic one-in-100 correlation
keys; flush exact stage summaries at process end.

**Rationale**: Full per-operation logging at up to 2000 aggregate
publications/s would risk making Boost.Log the bottleneck. At the lowest rate,
one-in-100 yields about 120 measured samples per peer per per-publication stage
over 60 seconds, while exact counters retain full path-frequency accounting.

**Alternative rejected**: Log every span and later assume the logger had no
effect. A pure end summary was also rejected because it cannot show tail
distribution or trace residuals.

## Decision 5: Hard-gate profiling perturbation

**Decision**: Compare a clean uninstrumented historical binary, the profiled
binary with profiling disabled, and the same profiled binary with the exact
formal profile enabled in one fixed short 1000 pps/peer preflight. Block sealing
on the FR-009 bounds.

**Rationale**: A same-binary on/off pair measures enabled logging but cannot
measure the timers, counter updates, branches, and cache effects that remain
when logging is disabled. Profiling overhead cannot be corrected reliably after
the fact; both components must be bounded before formal evidence is consumed.

**Alternative rejected**: Subtracting logger time from measured spans. Logger
effects include cache, scheduling, I/O, and backpressure that are not additive.

## Decision 6: Use one Face/io_context execution thread per peer process

**Decision**: Each of two MiniNDN nodes runs one peer process. After
initialization, one Face/io_context thread in each process executes an
independent absolute-deadline application timer, synchronous `publish()`, all
Sync/fetch work, and subscription callbacks. No other thread calls NDN-SVS or
Face.

**Rationale**: README and the public API do not promise cross-thread safety,
the examples' claim is not covered by tests, and Spec 134 sanitizer evidence
shows races when it is stressed. Keeping publication on the I/O thread measures
the historical serial design without invoking shared-state races. The timer
records skipped releases and lateness, so blocking work reduces attempted rate
instead of being hidden by an adapter queue or catch-up burst.

## Decision 7: Execute exactly five formal cells

**Decision**: One ascending once-only cell at each requested rate with
10-second warmup, 60-second measurement, and 10-second drain.

**Rationale**: The user requested the five old-version rates. More formal cells
would repeat the Spec 131 mistake of spending time on an unnecessarily large
matrix.

**Statistical posture**: Thousands of operations within a cell support
descriptive stage distributions, but are not independent run replications.
No p-value or run-level confidence interval is claimed.

## Decision 8: Rank demand and rate-boundary evidence together

**Decision**: A bottleneck needs at least two agreeing signals: exclusive
service demand/share, rate-dependent latency/queue/path growth, or alignment
with attempted/delivered-rate degradation.

**Rationale**: A rare slow stage can own the maximum without limiting
throughput; a frequently called moderate stage can dominate service demand.

## Decision 9: Correlate only from existing identities

**Decision**: Use existing node/sequence/range identities for publication,
Mapping, and Payload paths. Sync Interest joins are reported only when existing
wire-derived fields and occurrence ordering yield one unique match; all joins
are labeled exact, ambiguous, or censored.

**Rationale**: Adding a trace component, nonce, or payload field would change
the historical wire behavior being measured. Ambiguous joins are useful as
counts but cannot support a network-wait or critical-path conclusion.

## Historical threading-contract gap

The README is silent; the public header is silent; examples claim thread safety
and invoke publication across threads; tests are single-threaded; source uses
partial locking without a complete ownership policy. Spec 134's sanitizer
evidence therefore demonstrates a documentation/test/implementation mismatch,
not a defect on the formal single-I/O-thread path. Spec 133 preserves that
evidence but neither repairs nor exercises the disputed cross-thread path.

## Setup And Outputs

- **Entry workflow**: See [quickstart.md](quickstart.md).
- **Environment**: Separate Spec 133 worktree; exact base; Boost 1.71; fixed
  MiniNDN topology and CPU allowance.
- **Expected outputs**: Subject manifest, overhead receipt, five cell receipts,
  raw profile/event logs, stage summary CSV, grouped critical-path CSV,
  bottleneck ranking CSV/Markdown, campaign summary, and limitations.
- **Primary success threshold**: Complete valid attribution and an evidence-
  backed bottleneck ranking or explicit inconclusive verdict; not a required
  performance improvement.
