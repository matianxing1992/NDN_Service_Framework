# Feature Specification: DI Closed-Loop Workload Campaign

**Feature Branch**: `Experimental`

**Created**: Historical feature; audited 2026-07-21

**Status**: Accepted implementation; historical measurement evidence limited

**Input**: Extend the NativeTracer user driver and MiniNDN campaign harness from
one request to a small sequential closed-loop workload while preserving the
single-request default, current collaboration path, and smallest Qwen artifacts.

## Historical and Claim Boundary

Spec 012 predates the current Spec Kit document schema. Its implementation has
subsequently been extended by later DI workload Specs, including concurrent and
open-loop work. This document describes only the original closed-loop contract:
one user process, one reusable ServiceUser session, and at most one outstanding
request at a time.

The original smoke and two-run campaign paths were under `/tmp` and are no
longer present. Their numerical values are retained below as **historically
reported**, not independently reproducible measured evidence. This audit does
not rerun, replace, or strengthen those results.

## User Scenarios & Testing

### User Story 1 - Run Sequential Requests in One Session (Priority: P1)

As a NativeTracer experimenter, I can request a positive number of sequential
collaboration operations through one started ServiceUser and receive a result
record for every attempted request plus one aggregate workload record.

**Why this priority**: This is the smallest workload extension beyond a single
request and preserves the existing synchronous collaboration API.

**Independent Test**: Run the driver with three deterministic requests and
verify one user/request session, no concurrent outstanding request, three
ordered per-request records when all succeed, and one final workload record.

**Acceptance Scenarios**:

1. **Given** `requests=N>0`, **When** all requests succeed, **Then** the driver
   submits them sequentially through the same started ServiceUser and emits N
   per-request records in submission order.
2. **Given** a request fails, **When** the closed-loop driver observes it,
   **Then** it records the failure, stops submitting later requests, and emits a
   failed aggregate with attempted/success/failure counts.
3. **Given** `requests=1`, **When** the workload completes, **Then** the existing
   aggregate execution marker and single-request interpretation remain valid.
4. **Given** zero or a negative request count, **When** arguments are validated,
   **Then** execution is rejected before network work begins.

---

### User Story 2 - Preserve Workload Metrics Through MiniNDN and Campaigns (Priority: P1)

As an experiment maintainer, I can pass request count through the MiniNDN
harness and layout campaign and retain consistent request, latency, makespan,
success, and throughput fields from run output to aggregate summaries.

**Why this priority**: Metrics have no value if the driver, harness, and
campaign interpret them differently.

**Independent Test**: Feed known per-request results through the driver summary,
MiniNDN parser, row writer, and campaign aggregator; verify exact counts and
nearest-rank percentile/makespan/throughput calculations.

**Acceptance Scenarios**:

1. **Given** per-request elapsed times and workload elapsed time, **When** the
   aggregate is produced, **Then** it reports request/success/failure counts,
   mean, nearest-rank p50/p95, makespan, and successful requests per measured
   second.
2. **Given** the aggregate execution marker, **When** MiniNDN parses it, **Then**
   the same fields appear in `summary.json` without silently falling back to a
   one-request interpretation.
3. **Given** multiple layout runs, **When** the campaign aggregates rows,
   **Then** request count and workload metrics remain attributable to assignment,
   runtime candidate, run index, and result directory.

---

### User Story 3 - Report a Bounded Closed-Loop Comparison (Priority: P2)

As a researcher, I can compare default and single-provider layouts under the
same small sequential workload without presenting the result as concurrent
queueing or general performance evidence.

**Why this priority**: Correct claim boundaries prevent a small diagnostic from
being mistaken for a statistically strong or production-scale result.

**Independent Test**: Inspect the retained command/configuration and summary
table. Verify both layouts use the same request count and role-delay setting,
and that the interpretation states sequential closed-loop, small sample, and
missing durable raw evidence.

**Acceptance Scenarios**:

1. **Given** two compared layouts, **When** results are summarized, **Then**
   workload parameters, run count, request count, makespan, p95, and throughput
   are shown for both.
2. **Given** sequential submission, **When** findings are interpreted, **Then**
   no claim is made about concurrent outstanding requests, provider queueing,
   population confidence, steady state, or production performance.
3. **Given** the original `/tmp` artifacts are unavailable, **When** the feature
   is audited, **Then** numerical results are labeled historically reported and
   not upgraded to verified measured evidence.

### Edge Cases

- `requests=1`, zero, negative, or larger than the short campaign value.
- First, middle, or final request fails; later sequential requests must not be
  counted as attempted when they were never submitted.
- Makespan is zero or missing, so throughput cannot be computed.
- Percentile inputs are empty or contain one sample.
- Aggregate marker is present but per-request record count disagrees.
- MiniNDN summary is incomplete, stale, or belongs to another assignment.
- One campaign run fails and must remain attributable rather than being silently
  omitted or replaced.

## Requirements

### Functional Requirements

- **FR-001**: The user driver MUST accept a positive request count and
  preserve one as the default.
- **FR-002**: Closed-loop mode MUST submit requests sequentially through the
  same ServiceUser lifecycle with no more than one outstanding request.
- **FR-003**: The driver MUST emit one ordered per-request JSON record for every
  attempted request and stop later submissions after the first failed request.
- **FR-004**: The driver MUST emit explicit workload and backward-compatible
  aggregate execution records containing request, success, and failure counts.
- **FR-005**: Workload summary MUST report makespan, arithmetic mean,
  nearest-rank p50/p95, minimum, maximum, payload bytes, and successful-request
  throughput using the measured workload interval.
- **FR-006**: MiniNDN MUST pass the request count to the driver and preserve all
  workload metrics and their units in the run summary.
- **FR-007**: The layout campaign MUST pass request count to every run and retain
  assignment, resolved runtime candidate, run index, result directory, counts,
  workload metrics, execution status, and available Provider timing/capacity
  attribution.
- **FR-008**: Existing one-request aggregate parsing MUST remain compatible.
- **FR-009**: The feature MUST NOT add concurrent outstanding collaboration,
  change Provider scheduling, change the C++/pybind collaboration API, change
  wire protocol, or change model artifacts.
- **FR-010**: Result interpretation MUST identify sequential closed-loop mode,
  exact workload parameters, run/sample count, failed runs, and evidence level.
- **FR-011**: Missing durable raw artifacts MUST be reported as an evidence gap;
  historical values MUST NOT be represented as newly verified measurements.

### Key Entities

- **PerRequestResult**: Ordered request outcome with status, elapsed time,
  payload size, error, and request-specific evidence.
- **WorkloadSummary**: Aggregate counts, latency distribution, makespan,
  throughput, mode, and embedded per-request outcomes.
- **MiniNDNRunSummary**: One topology/layout run with workload and Provider
  attribution.
- **CampaignRow**: One attributable layout repetition.
- **CampaignSummary**: Per-layout aggregate over retained rows.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Deterministic tests with N successful requests produce exactly N
  ordered per-request records and one workload plus one compatible aggregate
  record, with at most one request outstanding.
- **SC-002**: Request-count validation rejects every non-positive value before
  network execution and preserves default `N=1` behavior.
- **SC-003**: Known latency fixtures produce exact nearest-rank p50/p95,
  makespan, counts, and throughput values through driver, MiniNDN, and campaign
  layers without unit or field drift.
- **SC-004**: Failure fixtures stop after the first failure and report attempted,
  success, and failure counts without counting unsent requests.
- **SC-005**: Maintained syntax and focused workload/parser tests pass against
  the current source.
- **SC-006**: Historical comparison documentation states 2 runs per layout,
  3 sequential requests per run, 75 ms per-role delay, both assignments, and
  the absence of durable raw artifacts.
- **SC-007**: No document claims concurrent queueing, statistical significance,
  steady-state behavior, or production generality from Spec 012.

## Historically Reported Results

The original document reported this full-network smoke command:

```bash
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --assignment default --role-execution-delay-ms 75 \
  --requests 3 --out /tmp/ndnsf-di-closed-loop-default-smoke \
  --provider-check-timeout 60
```

It reported three successful requests, makespan `1194.105 ms`, p95
`522.855 ms`, and throughput `2.512 rps`.

The original two-run-per-layout campaign reported:

| Assignment | Runtime candidate | Runs | Requests/run | Makespan mean ms | Makespan p95 ms | Workload p95 mean ms | Throughput mean rps |
|---|---|---:|---:|---:|---:|---:|---:|
| `default` | `shared-backbone-current` | 2 | 3 | 1128.025 | 1137.698 | 480.911 | 2.660 |
| `single-provider` | `single-provider-serial` | 2 | 3 | 1231.884 | 1260.203 | 489.938 | 2.437 |

These values imply a historically reported `103.859 ms` lower mean makespan
and about `9.2%` higher throughput for the default layout in that small setup.
Because the raw `/tmp` artifacts are absent, this is preserved as provenance,
not accepted as independently verified current evidence.

## Assumptions

- Later concurrent/open-loop features are outside Spec 012 and must not be used
  to retroactively strengthen its claims.
- Existing model artifacts, topology, security setup, and synchronous
  collaboration behavior are dependencies, not outputs of this feature.
- Auditing this historical feature does not authorize rerunning its campaign or
  changing implementation code.
