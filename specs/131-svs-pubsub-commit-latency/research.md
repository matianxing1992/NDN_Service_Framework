# Research and Experiment Decisions

## Decision 1: Compare two commits, not a 2x2 runtime factorial

**Decision**: Use `a9944019f76791773604999f00128057b9534ace` as the final
pre-feature subject and `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` as the
latest subject. Run the complete old block first.

**Rationale**: This is the comparison requested by the user and represents the
upgrade decision a maintainer actually faces.

**Alternatives rejected**:

- A 2x2 async on/off x threading on/off matrix answers component attribution
  but does not compare the old source against latest source.
- Comparing only `a994401` against `15d1bc6` improves causal isolation but does
  not evaluate the latest implementation and omits later correctness repairs.

**Limitation**: All seven intervening commits are part of the treatment bundle.
The final report must not attribute the whole effect solely to async or threads.

## Decision 2: Pin the last commit before both feature commits

**Decision**: The baseline is `a994401`, whose immediate descendants introduce
receive parallelization (`a8a9656`) and ordered async/parallel production
(`15d1bc6`).

**Source evidence**: Git ancestry shows the ordered path
`a994401 -> a8a9656 -> 15d1bc6 -> ... -> 6bb3454`. Historical header
inspection shows no `publishAsync()` or parallel Sync configuration in
`a994401`; current source exposes `publishAsync()`, `getSVSync()`, and both
parallel worker APIs.

## Decision 3: Match the V2 wire profile

**Decision**: Force the latest subject to V2 with 1 ms Interest lifetime,
500 ms suppression, 30 s periodic timeout, and 0.1 jitter; disable Sync
batching.

**Rationale**: The baseline predates selectable V3. Current source resolves V2
to the historical 1 ms/500 ms values. Holding the wire profile and timers
constant removes a large avoidable confound while retaining the requested
latest source.

**Alternative rejected**: Latest-default V3 would combine wire/signature size,
bootstrap identity, and timer changes with async/parallel behavior.

## Decision 4: Measure PubSub delivery and state synchronization separately

**Decision**: Primary delay is scheduled publication/API entry to public
subscription callback. Secondary delay is scheduled publication to the first
update callback covering that sequence.

**Rationale**: A StateVector update alone does not mean the PubSub application
received the payload. Reporting both separates state discovery from complete
application delivery without requiring private hooks.

## Decision 5: Use a shared monotonic raw clock

**Decision**: Publisher and subscriber timestamp with
`CLOCK_MONOTONIC_RAW`; a preflight confirms comparability across their MiniNDN
namespaces.

**Rationale**: Both processes run on one kernel. Monotonic raw time avoids NTP
or wall-clock adjustments and permits true one-way timestamps without network
clock synchronization.

**Alternative rejected**: Wall-clock timestamps can jump. RTT/2 hides
directional queueing and is not the requested publication-to-subscription path.

## Decision 6: Use a small fixed payload and one publisher/subscriber

**Decision**: Publish deterministic 256-byte values in a two-host, zero-loss,
100 Mbps, 10 ms one-way topology.

**Rationale**: The experiment targets PubSub synchronization throughput, not
segmentation or multiple-writer conflict. The payload stays within the small
publication path available in both commits and avoids data-size mix effects.

**Alternative rejected**: A size sweep or segmented corpus would multiply the
matrix and mix fetch/reassembly behavior into this first commit comparison.

## Decision 7: Use an independent high-resolution open-loop pacer

**Decision**: Run the Face event loop on a dedicated thread and schedule each
publication from an absolute monotonic epoch on a separate high-resolution
pacer thread. The pacer directly calls a common thread-safe adapter that uses
`io_context::post`; adapter acceptance is the attempted-load boundary, and the
pinned `publish()`/`publishAsync()` executes on the Face thread. ASan confirmed
that direct concurrent entry into the historical API otherwise double-frees an
`ndn::Scheduler` event node. The pacer skips and counts a release slot more than
two periods late instead of running an unbounded debt-repayment loop. Every
actual wake time is retained so bounded late sends remain visible. Face
scheduler callbacks never generate offered load.

**Rationale**: Closed-loop publishing lowers offered load when the old API
blocks and can falsely make it appear stable. Relative sleeps accumulate drift;
unbounded catch-up bursts create a different workload after a stall.

## Decision 8: One direct cell per rate and 60-second measured windows

**Decision**: One cell per subject-rate pair, each with 10 s warmup, 60 s
measurement, and 10 s drain. Before sealing the formal campaign, each subject
must independently demonstrate 1000 pps attempted rate within ±2% in a
non-formal MiniNDN smoke.

**Rationale**: The user requested exactly five old and five latest runs. The
1000 pps admission gate proves the corrected harness can generate the highest
offered load before any formal evidence is consumed. The total formal matrix
is 10 cells.

**Statistical posture**: Report direct descriptive old/latest differences and
the rate-response curve. One observation per subject-rate cannot support a
replication-based confidence interval, variance estimate, or p-value.

## Decision 9: Preserve loss in the latency result

**Decision**: Report delivered-only percentiles alongside delivery ratio and a
deadline-capped tail distribution that assigns missing items the drain
deadline.

**Rationale**: Under overload, only fast survivors may arrive. A latency-only
summary would reward loss and could reverse the conclusion.

## Decision 10: Execute sequential five-cell version blocks

**Decision**: Run all five baseline rates in ascending order, then all five
treatment rates in the same order.

**Rationale**: This satisfies the requested old-first order. Matched schedules,
the same shared four-CPU affinity, fresh namespaces, and process/resource telemetry reduce
but do not eliminate temporal drift or cross-process CPU contention.

**Limitation**: Version is confounded with execution time. The report must list
available load drift evidence and avoid a stronger claim than the design
permits. Host frequency and temperature were not captured, so thermal drift is
an acknowledged residual confound.

## Decision 11: External driver, immutable bases, and temporary Boost branches

**Decision**: Store the driver and runners in the NDNSF repository. For each
pinned NDN-SVS base, create a temporary local build branch whose only commit is
the same canonical `wscript` change from the Boost 1.74 gate/diagnostic to
1.71, then compile the driver separately against those clean patched
worktrees. Prove through patch, link, and process manifests that the runtime
delta is not changed, all Boost linkage is 1.71, and NDNSF is not used.

**Rationale**: A single external consumer bridges public API availability. The
small local build commit is required because both pinned sources reject the
host's Boost 1.71 before compilation; making it byte-identical, hash-bound,
unmerged, and path-restricted prevents it from becoming an uncontrolled
treatment difference.

## Decision 12: Descriptive comparison rule

For each rate, compare the one directly matched old/latest observation and
report the delivery and p95 differences. Label:

- `improved` only when both subjects sustain the rate, treatment delivery is
  not worse, and treatment p95 is at least 5% lower;
- `regressed` under the symmetric adverse conditions; or
- `inconclusive` otherwise.

These labels are descriptive classifications of these observations, not
inferential claims about a population of runs.

Also report each subject's highest rate satisfying ±2% achieved rate, at least
99% delivery, zero unaccounted observations, and (where available) zero worker
job drops. These are interpretation rules, not a requirement that latest win.
