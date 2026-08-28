# Implementation Plan: NDNSF External Bug Report Corrections

**Branch**: `Experimental` | **Date**: 2026-07-15 | **Spec**: [spec.md](spec.md)

**Input**: Peter's five-item bug report, limited by the corrected Spec 112 scope.

## Summary

Spec 112 fixes only the reported defects. Defects 1 and 2 are treated as one
ndn-svs segmentation/oversize workstream; defects 3, 4, and 5 remain separately
testable Python Targeted, request-deadline, and OpenABE-lifetime workstreams.
The existing NDNSF large-response reference mechanism is frozen and disabled
only inside the diagnostic roles so the MiniNDN test reaches the reported SVS
path. No new transport profile, public publication API, failure protocol,
large-object design, 5% loss study, DI work, Docker work, or iTiger work is part
of this feature.

## Technical Context

**Language/Version**: C++17, Python 3.8, local GCC 9.4 compatibility

**Primary Dependencies**: ndn-cxx 0.9.0, NFD, local ndn-svs, NAC-ABE/OpenABE,
RELIC, pybind11 `ndnsf`, MiniNDN/Mininet, Boost 1.71 local baseline

**Storage**: Existing in-memory ndn-svs publication/fetch state and immutable
local evidence directories only

**Testing**: Rebuilt ndn-svs unit tests, focused NDNSF C++/Python regressions,
subprocess lifecycle tests, and 0% loss MiniNDN Controller/Provider/User cells

**Target Platform**: Local Ubuntu/MiniNDN; the reporter's aarch64/NixOS/Wi-Fi
environment is source evidence, not a required Spec 112 execution platform

**Performance Goals**: None. This is a correctness, liveness, and lifecycle fix.

**Constraints**: Every packet at most 8800 B; secure tokens remain mandatory;
existing response-reference production behavior remains unchanged; failures and
negative evidence are retained; each candidate/cell runs once; current user
changes are preserved; local tests build on Boost 1.71

**Scale/Scope**: The five email defects, one necessary ndn-svs build-gate repair,
focused tests, a bounded 0% MiniNDN matrix, and completion evidence

## Constitution Check

*GATE: PASS before research and after design.*

| Principle | Design response | Gate |
|---|---|---|
| Canonical Dynamic Runtime | Uses current unified normal/Targeted APIs and no legacy Direct/generated path | PASS |
| Security Is Part Of Data Path | Tokens, permissions, NAC-ABE routing, Provider authorization, and replay rejection remain mandatory | PASS |
| CodeGraph First | Current SVS, timeout, binding, and teardown paths were traced before revision | PASS |
| Spec-Driven Durable Work | The five cross-repository corrections have explicit requirements, tasks, and evidence | PASS |
| Verify With Right Scope | Focused tests precede 0% MiniNDN acceptance; no host-NFD result closes the feature | PASS |

Additional gates:

- A strict Spec Kit audit `BLOCK` prevents implementation.
- No pre-fix reproduction uses a stale binary.
- No current-code change is made for email defect 3 or 5 if the new regression
  already passes.
- No MiniNDN result is admissible unless the diagnostic roles record
  `NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1` and no large-reference event.
- Any changed source, dependency, build, configuration, or test script produces
  a new candidate; a failed cell is never overwritten or silently rerun.

## Code Reality And Defect Mapping

| Email defect | Current code fact | Planned decision |
|---|---|---|
| 1. Segmented responses fail/degrade Provider | ndn-svs uses fixed `MAX_DATA_SIZE=8000`, signs inner segments, embeds them in signed outer Data, and has incomplete failure cleanup | Reproduce with the existing NDNSF externalization bypass, then repair final-wire sizing, commit ordering, exception containment, and cleanup in ndn-svs |
| 2. Oversized response aborts Provider | ndn-cxx caps packets at 8800 B; some posted `Face::put`/encoding paths can escape the request-handler catch boundary | Add exact-boundary tests and contain all errors on the owning event-loop boundary; never advertise unreadable state |
| 3. Python Targeted unusable | Current binding already forces tokens on and registers `NormalAndTargeted` | Add real-binding positive and token-negative regressions; change binding only if those current tests fail |
| 4. Targeted ignores timeout | `PublishRequestV2` occurs before `scheduleRequestTimeout`, so admission/publication can escape the caller deadline | Create/schedule the absolute deadline immediately after request acceptance and arbitrate response/timeout exactly once |
| 5. OpenABE exit crash | Current NAC-ABE destructor is empty and a process-wide executor exists, but this is not lifecycle evidence | Run repeated initialized subprocess exits; make the smallest lifetime change only if a current failure reproduces |

## Architecture And Ownership

| Concern | Owner | Boundary |
|---|---|---|
| Final inner/outer wire sizing, packet preparation, sequence advertisement, posted error containment, segment-fetch cleanup | `../ndn-svs` | Generic SVS transport; no NDNSF service policy |
| Existing diagnostic bypass and normal/Targeted service-response integration | NDNSF `ServiceProvider`/`ServiceUser` | Do not redesign the existing large-response reference path |
| Total Targeted deadline and exactly-once response/timeout callback | NDNSF `ServiceUser` | One native contract shared by C++ and Python |
| Normal/Targeted registration and secure token defaults | Python binding | Verify current behavior; no tokens-off production API |
| OpenABE process lifetime | `../NAC-ABE` | Fix only current reproducible teardown failures |
| Candidate identity, MiniNDN ownership, immutable results | `Experiments/` | Validation infrastructure only |

## Workstream A: Segmentation And Oversize Correction

The test Provider sets the existing
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1`; this is diagnostic setup, not a
new response-transport mode. Requests therefore reach the same
`SVSPubSub::publish -> segment preparation -> insertDataAtSeq -> outer sync Data`
path identified in the email.

The correction preserves existing public ndn-svs publication signatures:

1. Build the actual inner segment name, metadata, final block, and signature.
2. Build/sign the corresponding outer sync Data shape and check its final wire
   size against `MAX_NDN_PACKET_SIZE`.
3. Reduce the candidate content slice and repeat when the final outer packet is
   too large; reject only when even a zero-content packet cannot fit.
4. Complete fallible packet preparation before an irreversible asynchronous
   sequence reservation becomes caller-visible. If an internal tentative
   reservation is used, failure rolls it back under the same ordering owner
   before any later sequence can commit; a failed publication cannot create a
   visible gap or advance the sync state past unreadable data.
5. Prepare and store the complete logical publication before advancing or
   advertising its sequence state. Existing `publishAsync` returns its historical
   sequence only after the publication has crossed this safe preparation seam;
   the public signature and wire format remain unchanged.
6. Catch encoding/signing/`Face::put` errors inside posted/event-loop callbacks,
   discard incomplete prepared state, and keep the loop alive.
7. Give validation failure and timeout paths deterministic cleanup and avoid
   references to stack-owned callback state.

This plan adds no remote receipt protocol and does not claim that local storage
proves requester delivery. Existing API return types are not changed.

## Workstream B: Python Targeted Verification

The real compiled binding—not a mock fixture—must prove:

- one registered handler serves normal and Targeted requests;
- Provider and User keep tokens enabled;
- bootstrap and known-Provider fast path complete;
- missing, mismatched, consumed, and replayed tokens fail closed;
- no `set_use_tokens(false)` production escape hatch is added.

If all tests pass before source edits, defect 3 is closed as already corrected in
the current version and only evidence/documentation changes are permitted.

## Workstream C: Targeted Deadline

`timeout_ms` becomes one absolute deadline created and scheduled immediately
after the request is accepted and pending state is recorded. Admission,
bootstrap/refill, publication, and response wait all consume this same budget.
Response and timeout use one terminal transition; the winner invokes exactly one
callback and cleans pending/token/timer state. Publication exceptions do not
cancel the deadline path. Late responses are ignored after accounting and cannot
invoke the application again.

Spec 112 does not introduce cancellation, explicit remote failure, or a second
status protocol; the public observable outcomes in scope are the existing
response callback and timeout callback.

## Workstream D: OpenABE Lifetime

The existing process-wide executor and empty `ABESupport` destructor are treated
as a hypothesis, not a completed fix. Repeated subprocess tests initialize and
use NAC-ABE before normal and controlled exits. If the current code passes, no
teardown source is changed. If it fails, the smallest correction must keep
OpenABE initialization/use on its owning process-wide thread and avoid
`ShutdownOpenABE` from application/static destructors. A Boolean guard alone is
not accepted as proof because it cannot correct RELIC thread/static-destruction
ordering.

## Evidence Plan

### Experiment design review (ARS experiment-agent, plan mode)

| Element | Fixed design |
|---|---|
| Objective | Determine whether the current/fixed stack closes each of the five reported defects on its real path |
| Hypothesis | The final candidate meets every exact correctness/liveness threshold; a failing observation falsifies that defect's completion claim |
| Independent variables | Candidate (pre-fix/final), invocation mode (Normal/Targeted), and SVS publication mode (sync/async) |
| Dependent variables | Byte equality/completion, final packet size, Provider liveness/post-burst health, timeout callback count/error, and process exit signal |
| Controls | Same topology, 0% configured loss, routes, NFD log level, payload order, timeout, forced externalization-disable flag, and one Provider epoch where declared |
| Confounds excluded | Stale binaries, mixed candidates, Provider restart, automatic large-response reference, Wi-Fi/loss variation, and undeclared reruns |
| Sampling rationale | Boundary/burst tests are deterministic pass/fail regressions, not population estimates; 100 three-role lifecycle cycles are a fixed stress gate, not a statistical power claim |
| Analysis | Exact thresholds only; no p-values, effect-size claims, throughput claims, or inference from a partially completed matrix |
| Monitoring | Process-alive checks, per-cell wall timeout, disk guard, owned process tree, immutable logs, and machine-readable terminal summaries |

### Pre-fix candidate

- Rebuild ndn-svs and relevant tests against Boost 1.71.
- Run the existing unit suite once.
- Run the four 0% MiniNDN combinations: normal/Targeted × synchronous/asynchronous
  SVS publication, with forced inline-SVS response handling and sizes
  `64, 4000, 5000, 6500, 8000, 16000`.
- Preserve failures exactly as observed; do not repair and rerun the same
  candidate/cell.

### Final candidate

- Repeat the four boundary combinations once under a new candidate.
- Run one 80×8-KB burst followed in the same Provider epoch by 10×64-B and
  12×4-KB checks.
- Run real-binding Python normal/Targeted token tests.
- Run degraded/absent-Provider Targeted timeout/race tests.
- Run 100 initialized lifecycle cycles, each containing Controller, Provider,
  and User (300 role exits total).

There are no 5% loss, production-reference, or direct exact-name-object cells.

## Project Structure

### Documentation

```text
specs/112-ndnsf-segmented-reliability/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── segmented-publication.md
│   ├── request-terminal-outcome.md
│   ├── security-lifecycle.md
│   └── campaign-evidence.md
├── checklists/requirements.md
├── tasks.md
└── traceability.md
```

### Source And Tests

```text
../ndn-svs/ndn-svs/svspubsub.cpp/.hpp
../ndn-svs/ndn-svs/svsync-base.cpp/.hpp
../ndn-svs/tests/unit-tests/svspubsub.t.cpp
../ndn-svs/wscript
ndn-service-framework/ServiceUser.cpp/.hpp
pythonWrapper/src/ndnsf/_ndnsf.cpp
pythonWrapper/ndnsf/service.py
tests/unit-tests/generic-dynamic-api-targeted.t.cpp
tests/python/test_ndnsf_targeted_python_api.py
tests/python/test_spec112_targeted_timeout.py
tests/python/test_spec112_nac_abe_exit.py
examples/python/segmented_response_provider.py
examples/python/segmented_response_user.py
Experiments/NDNSF_Segmented_Response_Minindn.py
../NAC-ABE/src/algo/abe-support.cpp
../NAC-ABE/tests/unit-tests/abe-support.t.cpp
```

**Structure Decision**: Fix each defect at its existing lowest reusable owner.
No new library, public abstraction, wire namespace, or response mechanism is
introduced.

## Rollback And Stop Rules

- Each implementation workstream is a separate candidate and can be reverted
  independently while retaining its failing/passing evidence.
- A current passing Python Targeted or OpenABE lifecycle regression stops source
  modification for that workstream.
- Any packet above 8800 B, Provider crash, mismatched build identity, MiniNDN
  ownership conflict, or audit `BLOCK` stops the affected cell/workstream.
- Negative results remain evidence. A code/configuration change continues with a
  new candidate and the next declared execution, never by overwriting history.

## Post-Design Constitution Check

PASS. The corrected design is narrower than the previous draft, preserves all
security invariants, keeps public APIs stable, uses MiniNDN for the actual
network path, and contains no unrelated transport or DI feature.
