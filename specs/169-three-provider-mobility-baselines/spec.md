# Feature Specification: Three-Provider Mobility Baselines

**Feature Directory**: `specs/169-three-provider-mobility-baselines`  
**Created**: 2026-08-03  
**Status**: In progress  
**Upstream Evidence**: `specs/163-di-collaboration-planning/evidence/may20-review-traceability.md` BASE-1

## Purpose

Replace the one-endpoint NSC and gRPC mobility comparisons with honest
three-Provider baselines while preserving the prior NDNSF measurements. The
campaign must expose the different recovery trade-offs rather than tune either
baseline toward a desired result.

## User Stories

### User Story 1 - Health-aware gRPC recovery (Priority: P1)

As a researcher, I can run a three-endpoint gRPC client that learns Provider
availability only through active protocol probes and request outcomes, then
fails over within one logical request and reports the control cost.

**Independent acceptance**: with A unavailable and B available, one logical
request succeeds through B, records the failed A attempt, a failover, probe
traffic, one terminal result, and no transparent retry or hedging.

### User Story 2 - Timeout-driven NSC recovery (Priority: P1)

As a researcher, I can run a three-Provider NSC client without a background
reachability table; it waits for the current Provider attempt to fail and then
tries the next Provider under the same logical request and deadline.

**Independent acceptance**: with A unavailable and B available, one logical
request succeeds through B after the declared A timeout, records one failover,
and rejects or ignores any late A callback.

### User Story 3 - Six-cell retrospective baseline remediation (Priority: P1)

As a paper author, I can execute exactly the NSC and gRPC cells at 100, 150,
and 200 m using the same deterministic availability schedule and workload,
without executing NDNSF or altering its frozen result.

**Independent acceptance**: a fresh campaign contains exactly six terminal
cells, complete command/source manifests, cell metrics and aggregate tables;
there is no NDNSF process, cell, or newly measured NDNSF value.

## Functional Requirements

- **FR-001**: The baseline campaign SHALL use three Providers for both NSC and
  gRPC and one requester.
- **FR-002**: The two baselines SHALL use one deterministic per-range Provider
  availability schedule derived from seed 20, with the measured workload
  beginning at the same schedule offset.
- **FR-003**: Clients SHALL NOT read an availability state file or simulator
  coordinate; protocol observations are their only availability evidence.
- **FR-004**: Every logical request SHALL have one 5-second absolute deadline,
  one stable logical request ID, at most one attempt per Provider, and one
  terminal outcome.
- **FR-005**: Both baseline clients SHALL use the same declared 200 ms attempt
  timeout unless a pre-registered all-in-range sanity gate rejects that common
  value for both systems.
- **FR-006**: gRPC SHALL use three prewarmed channels, active health probes,
  deterministic Provider rotation, explicit cross-Provider failover, and no
  hidden retry or hedging.
- **FR-007**: gRPC SHALL count probes separately from service RPC attempts and
  SHALL retain status, target, latency, failover, and Provider execution
  evidence.
- **FR-008**: NSC SHALL retain no persistent Provider reachability table and
  SHALL advance only after a Nack or attempt timeout.
- **FR-009**: NSC SHALL fence late callbacks from superseded Provider attempts
  and distinguish attempt timeouts from terminal logical-request failures.
- **FR-010**: The harness SHALL start and gate all three Provider processes for
  each baseline and SHALL never pass availability files to either client.
- **FR-011**: The harness SHALL provide a sub-10-second smoke mode that proves
  topology startup, three-Provider invocation and summary parsing without
  producing performance evidence.
- **FR-012**: Formal execution SHALL include exactly `grpc,nsc` at
  `100,150,200` m, 5 RPS, 60 measured seconds, 5 ms service delay, 5-second
  request deadline, 200 ms common attempt timeout, and no automatic rerun.
- **FR-013**: Each cell SHALL retain the exact command, source hashes, mobility
  trace, logs, structured metrics and terminal status in a unique output path.
- **FR-014**: The campaign SHALL produce `campaign-summary.json`,
  `campaign-runs.csv`, and `campaign-cells.csv` and validate that all six
  scheduled cells appear exactly once.
- **FR-015**: Application-level operations and wire packets/bytes SHALL remain
  separate metrics; the campaign SHALL NOT equate a TCP segment with one RPC
  message.
- **FR-016**: Frozen NDNSF source and result inputs SHALL not be run, rewritten,
  or presented as trace-paired evidence when the original trace is unavailable.

## Edge Cases

- All Providers are unavailable until the global deadline.
- A Provider becomes unavailable after accepting work and later resumes.
- A probe is stale while an RPC succeeds, or a probe succeeds just before the
  Provider becomes unavailable.
- A superseded NSC or gRPC attempt produces a late response.
- Startup fails before the measurement barrier.
- A cell crashes or times out; it is retained as the terminal result and is not
  silently rerun.

## Assumptions

- The service workload is idempotent and side-effect free; duplicate Provider
  executions are measured rather than hidden.
- The old NDNSF mobility values are historical point estimates. Their original
  raw trace, latency distribution and traffic counters are not available.
- The current coverage harness models Provider application availability; it is
  not evidence of physical Wi-Fi disassociation or TCP handoff.

## Success Criteria

- **SC-001**: Focused tests prove A-to-B, A/B-to-C, all-unavailable, deadline,
  late-callback and accounting behavior for both clients.
- **SC-002**: One gRPC and one NSC smoke finish within 10 seconds of measured
  workload and emit `SMOKE_OK` plus a parseable terminal summary.
- **SC-003**: Exactly six formal 60-second cells execute once and terminate with
  retained results, regardless of whether individual service outcomes are
  positive or negative.
- **SC-004**: Every completed cell schedules 300 logical requests at 5 RPS and
  reports success, terminal failure, attempt, failover and latency metrics.
- **SC-005**: The campaign manifest proves `systems=[grpc,nsc]`,
  `ranges=[100,150,200]`, common workload parameters and zero NDNSF cells.
- **SC-006**: The frozen NDNSF input hashes recorded before implementation are
  unchanged after the campaign.

## Scope Boundaries

- No NDNSF rerun or NDNSF protocol change.
- No attempt to force gRPC success, message, or latency outcomes.
- No claim of statistical significance from a single run per cell.
- No external edge framework beyond NSC and gRPC in this campaign.
