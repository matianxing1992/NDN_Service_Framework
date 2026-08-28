# Research: Selection-Gated Boundary Repair

## Decision 1: Withdraw Central Ordering

**Decision**: do not implement or compare a central/global admission coordinator
in Spec 130. Use Provider-local atomic reservation, Requester-local ACK-window
selection, release-before-retry, finite leases and bounded full-jitter retry.

**Rationale**: the project contract is ACK/reservation first-come-at each
Provider, not a global request order. A central authority adds a new failure and
ownership domain without being required to close the observed Spec 129 gaps.
Local uniqueness plus no ownership across attempts prevents permanent
hold-and-wait; jitter addresses repeated collision probabilistically.

**Alternatives considered**:

- Epoch-scoped central coordinator: withdrawn because it changes the intended
  protocol rather than repairing the measured boundaries.
- Paxos/Raft or 2PC: unnecessary for the selected local-lease design and would
  introduce an unrequested distributed authority.
- Fixed retry: rejected because concurrent Requesters can remain synchronized.

## Decision 2: Preserve Late Liabilities After Window Closure

**Decision**: separate application response completion from post-window ACK
liability closure. Keep a bounded generic tombstone able to authenticate a late
ACK and ask the application for its opaque terminal targeted decision.

**Rationale**: `providerSelected` and pending-call completion are not evidence
that every Provider reservation has closed. Dropping a late ACK leaks capacity
until expiry. Core can generically retain delivery identity and publish a
targeted decision without interpreting reservation policy.

**Alternatives considered**:

- Ignore late ACK and rely only on lease expiry: safe eventually but violates
  prompt release and per-positive-ACK terminal decision.
- Reopen the ACK window: changes selection semantics and can produce a second
  winner.
- Keep the full pending call indefinitely: unnecessary memory/state retention;
  a minimal bounded tombstone is sufficient.

## Decision 3: Pin the Executable, Not Just the Timer

**Decision**: model a selected running role with an explicit execution pin.
Renewal/deadline failure first fences and stops the local executable; capacity
is released only after completion or confirmed stop.

**Rationale**: a timer proves only that a lease deadline passed. It does not
prove the worker stopped touching the GPU/model/resource. Reassigning capacity
before that fact creates simultaneous use and unsafe model replacement.

**Alternatives considered**:

- Release `COMMITTED` automatically at expiry: rejected as unsafe for long
  roles.
- Infinite lease while running: avoids premature release but creates an
  unbounded orphan after failure.
- Renewal-only ownership: insufficient when renewal packets are lost; local
  execution fencing must define the release boundary.

## Decision 4: Production Full-Jitter With a Release Barrier

**Decision**: the maintained NDNSF-DI client samples full-jitter exponential
backoff from system entropy after every prior reservation is released or
expired. Tests inject deterministic entropy; launchers do not implement a
parallel retry loop.

**Rationale**: deterministic unit coverage proves arithmetic but not runtime
wiring. The real client must own attempt fencing, total deadline, release wait,
fresh keys/tokens and terminal callbacks. Full jitter reduces synchronized
recollision while preserving an honest no-fairness claim.

## Decision 5: Direct-Predecessor Data Is the Only Stage Gate

**Decision**: NDNSF-DI owns the DAG and stage-data bindings. Source roles start
after their own selected pin/preparation. Downstream roles require authenticated
data from their declared direct predecessors only. ReadySet and
ExecutionActivate are not authority in this path.

**Rationale**: a global ready barrier adds latency and prevents pipeline
overlap. A plan factory and local probe are not evidence that maintained
client/provider entry points enforce the DAG. Real cross-Provider stage data is
required.

## Decision 6: Move Interpretation, Keep Generic Secure Primitives

**Decision**: generic NDNSF exposes application-neutral opaque metadata,
callbacks, targeted Selection transport, input-key wrapping and tombstone
lifecycle. NDNSF-DI interprets the DI capability, plan, roles, assignments,
reservation, retry, dependency and pin state.

**Rationale**: exact-target secure transport and late-message lifecycle are
reusable; GPU/model/DAG policy is not. Literal checks in Core make a foundation
application-specific and risk changing ordinary ACK semantics.

**Migration rule**: implement and exercise the NDNSF-DI adapter before removing
Core DI branches. Never run with two authoritative interpreters. Remove rather
than preserve the unaccepted central prototype.

## Decision 7: Real Fault Evidence

**Decision**: formal cells require distinct MiniNDN hosts/processes and one NFD
per host. Faults arise from provider timing, `tc netem`/routing, production-seam
payload suppression, or process signal/restart. Metrics come from messages,
journals and process events, never assigned expected counters.

**Rationale**: local deterministic probes are valuable unit tests but cannot
validate distributed timing, independent Requesters, routing, process death or
production caller wiring.

### ARS Experiment Methodology Note

- **Origin Skill**: experiment-agent
- **Origin Mode**: plan
- **Verification Status**: UNVERIFIED until the formal campaign runs
- **Objective**: falsify six protocol boundary claims, not compare throughput
- **Independent variables**: ACK timing, Requester overlap, resource conflict,
  renewal/cancel fault, retry collision, DAG shape, predecessor delivery fault
- **Dependent outcomes**: targeted decision closure, ownership overlap,
  release-before-retry, sampled delay, pin interval, downstream eligibility,
  branch overlap, terminal cause
- **Controls**: fixed topology per cell family, fixed ACK/deadline/lease
  configuration, unique identities, one invocation per frozen cell
- **Analysis**: exact safety assertions plus descriptive retry/progress metrics;
  no fairness, superiority or statistical-significance claim
- **Reproducibility boundary**: source/manifest hashes and immutable per-cell
  evidence permit inspection; formal cells are intentionally not rerun
