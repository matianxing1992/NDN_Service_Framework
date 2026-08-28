# Implementation Plan: Selection-Gated Boundary Repair and Real Fault Validation

**Branch**: `130-concurrent-fault-boundaries` | **Date**: 2026-07-21 |
**Spec**: [spec.md](spec.md)

## Summary

Repair the unverified Spec 129 boundaries without adding a central orderer.
Provider-local NDNSF-DI reservation remains the only scarce-resource authority:
a positive DI ACK follows a successful local tentative reservation, and each
Requester independently selects from its own ACK window. Incomplete attempts
close every reservation, observe receipt or bounded expiry, then retry through
the maintained client path using production full-jitter exponential backoff.

The repair also makes committed execution safe for long tasks: an executing
role holds a hard pin until completion or confirmed fenced abort; expiry cannot
silently expose the resource while code can still use it. The maintained
NDNSF-DI client/provider path will execute direct-predecessor DAG stages with
real stage data, not a launcher-local probe or a global ready barrier.

Finally, migrate DI interpretation out of generic NDNSF. Core retains generic
secure transport, opaque application extensions, exact-target terminal
Selection delivery, input-key confidentiality, and bounded late-ACK tombstones.
NDNSF-DI owns the capability literal, reservation, DeploymentPlan, assignments,
roles, retry, dependency, and pinning state.

The prior centralized `ConflictAdmissionCoordinator` plan, capability,
contract, 68-cell comparison, runner semantics, and `PASS` audit are
superseded. Existing partial code is a removal/migration target, not accepted
implementation. Spec 129 remains frozen and is hash-checked only.

## Current Code Reality and Repair Targets

CodeGraph and source inspection establish the following starting point:

| Boundary | Current fact | Required repair |
|---|---|---|
| Late ACK | `ServiceUser::selectLateAckAfterAckTimeout` rejects calls already marked selected, while earlier callback guards skip ACKs once a Provider is selected. | Preserve a bounded generic attempt tombstone and deliver one targeted application-produced negative decision for every valid late positive ACK. |
| DI ownership | Generic `ServiceUser.cpp`/`ServiceProvider.cpp` compare `DIReservationSelectionV1`; Core constructs/interprets DeploymentPlan members and roles. | Replace literal/policy interpretation with application-neutral opaque hooks; move DI construction/validation to NDNSF-DI. |
| Retry | `ContentionRetryController` is reached by tests, plan factory, and a deterministic experiment probe, not the maintained high-level request path. | Wire full-jitter retry into `DistributedInferenceClient`/request lifecycle with fresh attempt state and release barrier. |
| Dependencies | `DependencyDrivenExecution` is similarly fixture/factory/probe reachable. | Wire maintained client/provider stage-data callbacks and exact direct-predecessor gates. |
| Long pin | `AtomicReservationBook.expire()` releases `COMMITTED` entries by time without proving a running role stopped. | Add executable pin/renew/abort state so expiration fences execution before release. |
| Formal evidence | Withdrawn runner/scenario uses local deterministic probes and inferred counters for important faults. | Replace with separate MiniNDN hosts/processes, production messages, real delay/netem/process faults, and event-derived metrics. |
| Central prototype | `core/conflict_coordination.py`, deployment wrapper, tests, and runner/scenario callers implement the withdrawn design. | Remove exports/callers and delete or quarantine the prototype before behavior acceptance. |

## Technical Context

**Language/Version**: C++17 NDNSF runtime; Python 3.8+ binding and NDNSF-DI
application; Python experiment tooling

**Primary Dependencies**: ndn-cxx, NDN-SVS, NAC-ABE, pybind11, existing
`AtomicReservationBook`/journal, NDNSF-DI client/provider/DAG contracts,
MiniNDN/NFD, Linux `tc netem`

**Storage**: existing Provider reservation/runtime journals plus new bounded
Requester attempt tombstones and per-cell immutable JSON/CSV/event evidence; no
central ledger or conflict database

**Testing**: focused C++ and Python test-first changes, security and ordinary
NDNSF compatibility regressions, full source build, forced binding/package
rebuild, runner dry gate, and one fresh exact-once MiniNDN confirmation

**Target Platform**: Linux and MiniNDN; formal cells require separate hosts and
one NFD per host for Requesters and Providers

**Performance Goals**: no global serialization; late reservation release within
its bounded decision/expiry horizon; contention attempts stay within configured
deadline; fork branches demonstrate real overlap. No throughput superiority or
fairness claim.

**Constraints**: no central orderer; no Spec 129 rerun; no automatic live-cell
retry; safety before availability; no release of live executable; no UAV/codec/
model/workload special case; no plaintext protected input or exact assignment

**Scale/Scope**: two concurrent Requesters; two to four Providers; one
workload-neutral exclusive resource slot per contention domain; three-stage
chain and four-stage fork/join; sixteen frozen formal cells

## Constitution Check

| Principle | Result | Plan response |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Unified service names and V2 exact-target Selection remain; no generated/static API is introduced. |
| Security Is Part Of Data Path | PASS | NAC-ABE, tokens, signer/identity, replay, encrypted input and recipient assignment remain mandatory. |
| CodeGraph First | PASS | Current late-ACK, DI leak, retry/dependency caller, pin expiry, and central prototype paths were identified before planning. |
| Spec-Driven Durable Work | PASS | Goal reset, contracts, cohesive tasks, analyze, fresh audit, implementation, convergence and freeze are explicit. |
| Verify With Right Scope | PASS | Unit/security/build/binding plus real multi-host MiniNDN faults are mandatory. |
| Cohesive Outcome-Based Tasks | PASS | Each task closes one behavior with its tests, integration and evidence; formal execution remains an independent irreversible gate. |

## Selected Design

### 1. Provider-local reservation and Requester-local selection

There is no cross-request ordering service. A Provider's NDNSF-DI ACK handler
authenticates the request, evaluates its own finite capacity, atomically creates
a tentative reservation, and only then returns a positive ACK. A Requester
selects using its own fixed ACK window. Disjoint requests naturally overlap.
Conflicting requests receive local negative ACKs or unusable partial offers.

An incomplete attempt closes every positive reservation with exact-target
`NOT_SELECTED`; retry is forbidden until each release is acknowledged or the
Provider-authenticated finite expiry is reached. This removes hold-and-wait
between attempts. It can still collide repeatedly, so progress is bounded by
attempt count/deadline and described probabilistically.

### 2. Generic late-ACK lifecycle hook

Generic NDNSF retains a bounded post-window record keyed by requester, request
ID, attempt, service, Provider and application decision identity. Core decrypts
and authenticates late ACKs, but does not decide what a reservation means. An
application-neutral callback classifies whether a valid positive ACK needs a
terminal per-target decision and returns opaque authenticated decision context.
Core publishes/retries the targeted Selection and records delivery/receipt
idempotently. NDNSF-DI supplies `NOT_SELECTED` and its reservation binding.

The tombstone lifetime is derived from the maximum application-declared ACK
liability horizon, capped by configured safety bounds. It remains after normal
response callback closure, cannot reopen candidate selection, and is removed
only after all known liabilities close or the horizon elapses.
The record is preallocated only for the authorized Provider set and charged to
per-request, per-identity and global lifecycle quotas before REQUEST publication;
an active liability is never evicted early merely to accept a newer call.

### 3. Executing pin state machine

Provider reservation states become:

```text
TENTATIVE -> COMMITTED -> EXECUTING -> COMPLETING -> RELEASED
                         |              ^
                         -> STOPPING ---|
TENTATIVE/COMMITTED -> EXPIRED/ABORTED -> RELEASED
```

`EXECUTING` is a hard pin. Renewal extends its control deadline but does not
define ownership by itself. If renewal or execution deadline fails, the
Provider enters `STOPPING`, fences/cancels the worker and waits for local stop
confirmation. Only then may it release. Provider restart uses a new boot epoch;
restore must either prove the old executor dead or keep the resource
unavailable. A stale completion from an old attempt cannot release a new pin.

### 4. Production full-jitter retry

The maintained NDNSF-DI client owns the retry loop:

```text
delay ~ Uniform(0, min(cap, base * 2^(attempt-1)))
```

Production uses system entropy independently per Requester. Tests inject an
entropy function; fixed seeds are prohibited in maintained production entry
points. Before sleeping, the client closes all offers and satisfies the release
barrier. It then creates fresh request/attempt, token, input key, plan and
reservation bindings. Maximum attempts and one absolute deadline bound the
loop; delay is truncated by remaining deadline.

### 5. Production dependency execution

NDNSF-DI owns a canonical DAG and per-recipient projection. The maintained
client passes it through opaque framework extension data. A Provider validates
its exact role/assignment, commits its pin, prepares locally, and:

- starts a source role without waiting for other Providers;
- emits authenticated stage data with request/attempt/producer/consumer/role/
  sequence/chunk/digest bindings;
- starts a downstream role only when its direct predecessor set is complete;
- allows independent ready branches to overlap;
- aborts affected descendants on missing/tampered/stale predecessor data;
- emits one terminal result only from the declared terminal role.

No ReadySet or ExecutionActivate message is authoritative in the R1/Spec 130
path.

### 6. NDNSF/NDNSF-DI ownership migration

Behavior migrates before deletion:

1. Define generic opaque ACK-liability/targeted-decision/tombstone callbacks and
   bind them to Python without DI names.
2. Implement NDNSF-DI adapters that parse `DIReservationSelectionV1`, build the
   plan/assignment/decision payloads, and drive reservation/pin/retry/DAG state.
3. Move maintained DI client/provider callers to the adapters and prove parity
   with security/selection tests.
4. Remove Core literal checks, DI plan/member/role synthesis and DI-specific
   Provider validation branches.
5. Remove the withdrawn central coordinator module, wrappers, exports, tests,
   runner modes and manifest vocabulary after proving no maintained caller.

If the generic hook migration fails, rollback restores the previous source
revision as a unit; no mixed state may leave both Core and NDNSF-DI interpreting
the same DI decision. No compatibility alias is retained for the central
prototype because it was never accepted.

## Ownership Matrix

| Concern | Owner | Main implementation seam |
|---|---|---|
| Secure Request/ACK/Selection/Response transport | NDNSF | `ndn-service-framework/Service{User,Provider}.*` |
| Opaque application extension, late-ACK tombstone and exact-target decision delivery | NDNSF | generic C++ API plus `pythonWrapper/src/ndnsf/_ndnsf.cpp` and `pythonWrapper/ndnsf/service.py` |
| DI capability/reservation/plan/assignment interpretation | NDNSF-DI | `ndnsf_distributed_inference/{client,provider,plan,deployment}.py`, `core/` adapters |
| Provider capacity and execution pin | NDNSF-DI Provider | `core/deployment_control.py`, provider/runtime journal, native adapter where used |
| Full-jitter retry and attempt lifecycle | NDNSF-DI Requester | `core/recovery.py`, `client.py`, `plan.py` |
| DAG and stage-data eligibility | NDNSF-DI client/provider | `core/execution.py`, `client.py`, `provider.py`, native consistency adapter |
| Real fault scenario, runner and analyzer | Experiments | new Spec 130 runner/scenario implementation after prototype removal |
| Frozen baseline | Spec 129 | hash-read only; never invoked or modified |

## Security and Distributed Correctness Invariants

1. Generic NDNSF never assigns resource meaning to an ACK.
2. Every DI positive ACK has one Provider-local tentative reservation before
   publication; every such reservation reaches SELECTED or NOT_SELECTED.
3. A late ACK cannot reopen selection, but its reservation liability cannot be
   dropped before terminal decision/expiry.
4. No retry starts with live ownership from the prior attempt.
5. One Provider resource has at most one live owner.
6. `EXECUTING` capacity is never released until the old executable is confirmed
   stopped or completed.
7. Only authenticated direct predecessors can make a stage eligible.
8. Input keys and exact assignments are disclosed only to exact selected
   recipients and never appear in REQUEST, ACK, NOT_SELECTED, logs or evidence.
9. Attempt, Provider boot epoch, token and digest replay fencing survives delay,
   reorder, restart and retry.
10. Missing authority or stop evidence fails closed; unavailable is preferable
    to double execution.

## Experiment Design

### Objective and claims

The experiment tests six correctness boundaries, not comparative throughput.
Deterministic safety outcomes are exact assertions. Retry progress cells report
the observed attempts/delays/terminal outcome and do not infer fairness from a
single campaign.

### Formal sixteen-cell corpus

| Cell | Family | Scenario | Required real effect |
|---:|---|---|---|
| 1 | Late ACK | on-time two-Provider control | normal ACK-window messages |
| 2 | Late ACK | second positive ACK after another Provider selected | real Provider publish delay beyond ACK timeout |
| 3 | Late ACK | late ACK duplicate/reorder after response closure | real delayed duplicate publication/reordering |
| 4 | Concurrency | two Requesters, disjoint Provider resources | two requester processes progress concurrently |
| 5 | Concurrency | two Requesters, one exclusive resource | concurrent requests reach one Provider-local reservation winner |
| 6 | Concurrency | complementary partial reservations | two Providers and release-before-retry lifecycle |
| 7 | Pin | long role crosses initial committed lease boundary | real long-running provider handler plus contender |
| 8 | Pin | renewal message loss | real loss/suppression followed by local stop-before-release |
| 9 | Pin | cancellation/abort while running | real cancel/worker stop and later contender acceptance |
| 10 | Retry | collision followed by production jitter success | maintained client loop and system entropy |
| 11 | Retry | repeated collision to bounded exhaustion | maintained client loop reaches attempt/deadline bound |
| 12 | Dependency | real three-stage chain | stage payloads cross three Provider processes |
| 13 | Dependency | real fork/join overlap | stage payloads cross four Providers with measured branch overlap |
| 14 | Dependency | predecessor data loss/tamper | real production-seam suppression or link fault, no downstream execute |
| 15 | Boundary | ordinary non-DI positive ACK | no reservation/mandatory negative Selection |
| 16 | Boundary | DI secured path plus source/import negative scan | application-owned DI interpretation and zero Core literal/policy leak |

Every live cell uses unique Requester/Provider identities on separate MiniNDN
hosts with one NFD per host. The runner records PIDs, host-to-identity mapping,
routes, fault commands/events, source and manifest hashes, real message/event
lineage and exit codes. A cell is invalid—not passed—if required processes share
one host/NFD or if the named fault is evidenced only by a locally assigned
counter.

### Fault controls

- ACK lateness uses a real Provider publish delay configured before launch.
- Loss/reorder uses link-specific `tc netem` or explicit routing disruption
  when message-class isolation is possible; otherwise the production
  application seam suppresses the named authenticated publication and records
  both attempted and suppressed events.
- Crash/abort uses process signals and records PID/exit/restart epochs.
- No runner branch writes expected reservation, retry, pin, stage or rejection
  counts. Metrics are derived from process logs, message traces and journals.

### Evidence and stop rules

The runner freezes `experiment-manifest.json`, checks Spec 129 hashes, acquires
single-writer ownership, refuses existing output, and performs a dry gate before
formal execution. Formal cells run exactly once with no automatic or selective
retry. A harness-invalid cell remains recorded and blocks closure; it is not
replaced silently. Required artifacts are `campaign-summary.json`,
`campaign-runs.csv`, `campaign-cells.csv`, topology/process/fault manifests,
per-cell message and reservation/pin/dependency ledgers, and the analyzer
report. The campaign stops before first live cell if build/binding/security,
source hashes, topology isolation, fault capability, output ownership, or
manifest validation fails.

## Project Structure

```text
specs/130-concurrent-fault-boundaries/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── contracts/boundary-repair-contract.md
├── experiment-manifest.json
├── quickstart.md
├── tasks.md
└── audit*.md

ndn-service-framework/
├── ServiceUser.{hpp,cpp}
└── ServiceProvider.{hpp,cpp}

pythonWrapper/
├── src/ndnsf/_ndnsf.cpp
└── ndnsf/service.py

NDNSF-DistributedInference/ndnsf_distributed_inference/
├── client.py
├── provider.py
├── plan.py
├── deployment.py
└── core/{deployment_control,execution,recovery}.py

Experiments/
├── run_spec130_boundary_repair_matrix.py
└── NDNSF_DI_BoundaryRepair_Minindn.py

tests/
├── unit-tests/generic-dynamic-api-selection.t.cpp
└── python/test_spec130_*.py
```

**Structure Decision**: add only application-neutral lifecycle hooks to the
foundation; put all DI semantics in NDNSF-DI. The withdrawn central prototype
paths are removed rather than renamed into the new design.

## Implementation Phases

1. Supersede/remove central prototype entry points and freeze the new contract,
   manifest, source boundary and Spec 129 hashes.
2. Repair generic post-window ACK liability handling and migrate DI
   interpretation to NDNSF-DI adapters.
3. Repair Provider execution pinning and wire maintained client full-jitter
   retry.
4. Wire maintained multi-stage client/provider execution and stage-data path.
5. Replace the simulated runner/scenario with real multi-host MiniNDN faults and
   event-derived analysis.
6. Pass focused/security/full build/binding gates, then run one new exact-once
   sixteen-cell confirmation and close only against measured evidence.

## Complexity Tracking

No constitution exception is accepted. The generic late-ACK callback/tombstone
seam is justified because late secure message delivery is application-neutral;
all reservation and execution policy remains in the application. No central
authority, distributed transaction protocol, conflict graph, or second
resource ledger is added.
