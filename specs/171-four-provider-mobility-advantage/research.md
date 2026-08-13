# Research: Four-Provider Multi-AP Mobility Advantage

## Decision 1: Extend Spec169 rather than replace it

**Decision**: Keep Spec169's three-Provider six-cell campaign as a frozen
baseline and add a separate four-Provider campaign.

**Rationale**: The existing result is not trace-paired with the historical
NDNSF row. Rewriting it would falsely convert a corrected baseline into a new
NDNSF comparison.

## Decision 2: Generalize endpoint cardinality

**Decision**: gRPC and NSC accept four targets/prefixes, retain one attempt per
Provider per logical request, and enforce one global deadline.

**Rationale**: The objective tests multi-Provider diversity against sequential
retry. The gRPC client currently rejects anything other than three targets;
NSC already has request-scoped Provider-list parsing and attempt rotation.

## Decision 3: Use deterministic coverage-gated multi-AP traces first

**Decision**: Model a Provider as reachable when it is inside the union of
declared AP coverage discs; use the nearest AP for trace attribution.

**Rationale**: This makes NDNSF, gRPC, and NSC receive exactly the same
availability schedule and keeps the first smoke reproducible. The output must
not be described as physical RF association/handoff evidence until association
events are recorded.

## Decision 4: Register a harsh and moderate screening pair

**Decision**: Start with three APs, 100-m/8-m/s moderate and 75-m/15-m/s harsh
cells, using the same four-Provider traces for each system.

**Rationale**: A one-AP cell diagnoses correlated outage; the three-AP cells
test whether Provider diversity provides alternate reachable endpoints. The
two screening cells are small enough to decide whether formal repetitions are
warranted.

## Decision 5: Analyze reliability and cost together

**Decision**: Treat logical success, deadline misses, recovery gap,
attempts/request, failovers/request, latency, and duplicate execution count as
co-primary evidence.

**Rationale**: Four endpoints can improve gRPC success while increasing retry
traffic. A success-only plot would hide the tradeoff and cannot support a claim
about NDNSF's mechanism.

## Existing evidence boundary

Spec169's formal three-Provider gRPC-HC-3 cells report 90%/100%/100% success
at 100/150/200 m with explicit retry, while the older slide table reports a
single-endpoint gRPC row. This proves that the old table is not a fair primary
comparison; it does not prove the new four-Provider NDNSF claim.

## Decision 6: Formal repetition is conditional and stress-focused

**Decision**: Do not launch a full Cartesian sweep. The screening justifies a
formal repetition campaign only for the registered stale-health stress cell
(`75 m`, `15 m/s`, `300 ms` logical deadline, `100 ms` attempt timeout, `1 s`
health interval), plus one matched moderate control cell (`100 m`, `8 m/s`).

**Rationale**: The paired smoke was 25/25 for all three systems, and the harsh
500-ms screen was 24/25 NDNSF, 25/25 gRPC-HC-4, and 24/25 NSC-4. Neither
demonstrates an advantage. A separate 25-request stale-health replay was
25/25 NDNSF, 24/25 gRPC-HC-4, and 22/25 NSC-4, which is a useful hypothesis
signal but not a claim.

**Formal protocol**: Use identical traces and source hashes for all systems,
retain every cell, and run at least 10 paired seeds per condition before
deciding whether a larger repetition budget is warranted. Aggregate per-seed
logical-success differences with a paired bootstrap (or an exact paired
interval), report attempts/request and p95/p99 alongside success, and apply
SC-005 to the lower confidence bound. If the bound is not at least 10
percentage points above both sequential baselines, record
`NO_DEMONSTRATED_ADVANTAGE` and do not edit slide 32 or the paper table.

## Decision 7: Stop the all-selected repetition after a complete negative seed

**Decision**: Do not spend another long MiniNDN batch on `all-selected` after
the first complete stale-health seed unless a new selection policy or workload
is introduced.

**Evidence**: The paired seed produced NDNSF 297/300 with 620 Provider
executions, versus gRPC-HC-4 299/300 with 310 attempts and NSC-4 275/300 with
492 attempts. The earlier first-responding control also never exceeded gRPC in
its five completed seeds. Thus the registered success gate is not promising,
while `all-selected` exposes a clear duplicate-execution cost.

The seed remains retained as a complete, immutable negative mechanism result;
the aggregate report is explicitly `INCONCLUSIVE_MISSING_CELL`, not a
statistical claim. Any future formal campaign should first change a registered
factor (for example, a truly instrumented AP-handoff topology or a workload
where multiple valid responses are semantically required), rather than merely
adding repetitions to the same configuration.

## Decision 8: Separate strict sequential gRPC from health-assisted gRPC

**Decision**: Add an explicit `gRPC-SEQ-4` mode that keeps the four configured
endpoints but disables proactive health probes and health-directed initial
selection. Preserve the existing `gRPC-HC-4` mode as a secondary diagnostic.

**Rationale**: The requested hypothesis is about one NDNSF publication reaching
one of several currently reachable Providers versus a baseline spending the
same request deadline on endpoint attempts one at a time. The previous
`gRPC-HC-4` run concurrently probed all four endpoints and reordered requests
using fresh health state, which is a stronger adaptive baseline than the
strict sequential behavior being tested. Reporting both modes prevents an
implicit baseline downgrade while making the hypothesis test explicit.

## Decision 9: Register a single-active handoff stress trace

**Decision**: Add a deterministic `single-active-handoff` trace profile. At
each recorded epoch exactly one Provider is inside the declared AP coverage;
the active Provider rotates on a manifest-recorded period. All systems replay
the same trace, and clients still receive no trace or availability file.

**Rationale**: Random-waypoint traces often leave several Providers reachable
simultaneously, so sequential gRPC can succeed before spending its deadline.
The registered schedule isolates the causal condition in the hypothesis: at
least one Provider is reachable, but its position in a sequential endpoint
list matters. The profile is explicitly coverage-gated and cannot be described
as physical Wi-Fi association or handoff evidence.

## Decision 10: Make repeated handoff traces seed-distinct

**Decision**: Derive and record a deterministic Provider rotation order from
the repetition seed. Keep the one-active-Provider invariant and handoff period
unchanged, but require different seeds to produce different trace hashes.

**Rationale**: Repeating the exact same availability trace would be a weak
screening sample and could hide schedule-sensitive failures. Seed-dependent
ordering preserves the paired comparison while preventing accidental
pseudo-replication. The earlier two-seed pilot is retained as screening
evidence, but formal runs must use the corrected generator.

## Decision 11: Keep HC mobility setup valid

**Decision**: In the single-active profile, `gRPC-HC-4` may begin with
unavailable endpoints not prewarmed; it must retain health probes and report
health-directed selections instead of failing setup on an all-endpoints
prewarm requirement.

**Rationale**: Requiring every endpoint to prewarm contradicts the registered
coverage schedule and turns the secondary diagnostic into a setup test. The
10-second HC diagnostic completed 50/50 with 34 health-directed selections,
confirming that this mode is a useful labelled upper-bound comparison rather
than the strict baseline.
