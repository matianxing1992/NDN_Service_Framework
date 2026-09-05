# Implementation Plan: ACK-Driven YOLO Tiger Functional Slice

**Branch**: `Experimental` | **Date**: 2026-09-03 | **Spec**: [spec.md](spec.md)

Current implementation checkpoint (2026-09-04):
`evidence/t013-supervision-repair-20260904.md` continues the prior
`evidence/t014-tiger-path-audit-20260904.md`. Runtime supervision and same-tree
launcher execution are wired, with 44 focused checks and a new 408-file
source-only archive. T013 still owns real numerical/CUDA result production,
transient-helper and secret/scratch cleanup, and complete candidate binding;
T011 owns the live Y-N negatives. T014 remains BLOCK; no qualification gate or
scope change follows from these implementation checks.

T013 supervision repair: keep `run-ndnsf-yolo.sh` as the public in-image
entry and move its process bookkeeping into sibling `supervise-tiger.py`.
This replaces (not supplements) the shell supervisor. Preserve the current
Controller/publication → Repo → Providers → User barriers, monitor every
started child, and collect bounded process-group shutdown evidence including
groups whose leader already exited. A successful User marker alone must not
produce terminal PASS. Load the launcher, renderer and bootstrap from the
same sealed replay tree; bundle input remains read-only configuration/data.

## Revision 123: readiness repair and candidate invalidation

The revision-122 SIF reproduction exposed a Controller startup race before the
publication User could fetch NAC-ABE `PUBPARAMS`. The repair is deliberately at
the reusable Core/Python readiness boundary: Core `ServiceController::start()`
probes the real public-parameter Data endpoint on its Face before returning,
and the Python Native Controller waits on that result in both synchronous and
background startup. This does not alter ACK disposition, candidate
enumeration, placement ownership, or wire protocol semantics.

The behavior-affecting runtime change invalidates revision-121 exact-SIF
evidence. T013 owns the focused repair/rebuild; the next gates are one fresh
T014 convergence `PASS`, one current-source T015 local qualification, then a
new immutable SIF and exact-SIF replay. No old SIF/Tiger evidence is reused.

## Revision 124: completion order closes every known gap

The controlling completion item is the FR-008 protected-grant subsystem,
owned by T007 (authority + placement grant seam + Provider verify/unwrap)
and T010 (Y-N-E real grant mutation); T014 stays BLOCK until it closes.
The revision-121 S4 exact-SIF replay is invalidated by the revision-123
runtime repair; S4 reruns only from a fresh candidate after T014/T015.
The Controller readiness probe uses an independent second Face with a
random name suffix (commit `8b24911b`); a same-Face in-process Data
satisfaction cannot prove readiness. Executing T001--T020 in completion
order now closes every known open problem.

## Revision 112: shortest valid route to Tiger (historical; superseded by revision 123)

The previous plan made the first remote result depend on two models, three
distinct GPUs, two requests per model, the full local suite, a large negative
matrix, and final cross-model closure. That ordering optimized for final
qualification before proving deployability and is the main planning reason
Spec180 repeatedly stopped before Tiger workload entry.

The active plan now delivers one vertical slice:

```text
S0 native closure (-j2, one build tree)
 -> S1 signed role-correct YOLO candidate
 -> S2 local MiniNDN Y-A -> Y-B -> critical Y-N
 -> S3 convergence PASS + YOLO-relevant local gate
 -> S4 one local SIF + exact-SIF Y-B replay
 -> S5 one-node/one-GPU/four-Provider Tiger Y-B request
```

S5 uses four independent Provider processes. The three ONNX model roles share
the one allocated RTX GPU and record the same physical device identity; the
`Merge` Provider runs its declared CPU postprocessing. This is a functional
deployment result, not a multi-GPU or performance experiment.

Qwen3.6-27B, three-distinct-GPU placement, a second warm request, cache reuse,
and performance measurements are deferred to a later feature. Existing Qwen
handoff/reference material remains valid project context but is not on the
Spec180 critical path. Earlier plan revisions below are retained as diagnosis
history; revision 112 controls whenever they conflict.

To remove the unavailable production-key dependency from a functional test,
S1 creates one experiment-only catalogue/Provider trust set before candidate
sealing. Private keys remain outside Git and the SIF; public identities and
digests are frozen into the candidate. This tests the real signature-verification
path without claiming production PKI deployment or allowing runtime key
generation.

**Revision 110 execution diagnosis (2026-09-03)**: The feature is delayed
before any real NDNSF-DI request for two independent reasons: the native
runtime is not one ABI-identical closure, and the current candidate lacks a
role-correct registered signature plus a single immutable Provider-key/model
input set. This is compounded by the former task graph allowing audit,
negative-case, and release work to advance before one atomic Y-A request.
Focused tests and fail-closed preflight are useful evidence, but they are not
an experiment result. The private signing key must come from the registered
owner; do not generate a new trust root or repair a stale manifest in place.
The first next action is `G0-NATIVE` closure, followed by owner-supplied
`G0-CANDIDATE` closure and one Y-A terminal Response. No SIF, Tiger, or broad
matrix action is authorized before that response.

The first attempted Y-A launch also exposed a host closure defect: Python and
NAC-ABE resolved the repository `.local-boost171` `libndn-cxx`, while NFD and
the current NDN-SVS resolved a different `/usr/local` build with the same
soname. The resulting native-client socket EOF occurred before any request.
The runner now fails before MiniNDN with `WAITING_EXTERNAL_INPUT` when these
resolved files or hashes differ. The next implementation action is one
ABI-identical rebuild of NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python extension;
an ad-hoc `LD_LIBRARY_PATH` change is not an accepted repair.

**Revision 111 resource-boundary correction (2026-09-03)**: Local native
builds are resource-constrained implementation steps. The canonical command
must use Waf `-j2` (or an equivalent maximum of two compiler jobs), with at
most one active build tree. `-j8`, unconstrained `nproc` expansion, and
overlapping build trees are invalid inputs even when the host has swap. A
build request that exceeds the limit must be rejected before compilation; a
partial object tree is not evidence and cannot be promoted to G0-NATIVE.
Runtime Provider parallelism is a separate experiment concern and does not
authorize higher compiler concurrency.

**Input**: Feature specification from
`specs/180-ack-driven-cross-model-qualification/spec.md`

**Revision 110 execution discipline (2026-09-03)**: This plan is a gated
vertical slice, not a parallel checklist. Only the first failing gate is
active: G0-NATIVE closure, G0-CANDIDATE input closure, then G1 atomic Y-A,
then G2 shared/negative cases,
then T013 release/QWEN-F, T014 convergence, and the execution-only T015--T020
route. Boundary tests may close the current owner task, but they do not open a
later gate. This prevents repeated audit, preflight, SIF, and Tiger cycles from
being mistaken for a protocol experiment.

**Revision 107 input-status note (2026-09-03)**: The YOLO entrypoint now
reports missing/invalid pre-network inputs as `WAITING_EXTERNAL_INPUT` with
exit code 78, while failures after the MiniNDN case starts remain
`UNQUALIFIED`. This is evidence classification only and does not advance G0.

**Revision 106 implementation note (2026-09-03)**: T013's separate QWEN-F
entrypoint is now present and focused-tested. It remains an implementation
boundary: the signed external manifest, in-image object set, and live
two-request result are still required before T013/T014 can close.

## Summary

Reuse the existing request-first `AutomaticPlanningCoordinator`, canonical
artifact transport, signed role assembly, and native Provider runtime. A real
YOLO26 adapter submits a task without a Provider list, inspects its canonical
ONNX graph after ACK closure, chooses between adapter-certified atomic and
shared-backbone candidates, assembles selected roles on Providers, and verifies
the final result against one full-model oracle. After the finite local gates,
build one SIF and use it for exactly one Tiger YOLO request.

**Execution recovery decision (audit iteration 103; documentation revision 110)**: The next deliverable is one
real Y-A request that crosses Controller/Repo/Provider/User startup, signed
runtime-catalogue publication, encrypted input-reference publication,
authenticated ACK closure, Selection, Provider execution, and terminal
Response through `run_minindn_case()`. The current unconditional
driver stub has been replaced by a barriered launch path. The first blocking
line is now native closure, followed by candidate readiness: a fresh `DetectShard0/1` package and the
Provider's offer-signing key plus canonical local model path must be bound
before the path may run. No new general preflight, evidence schema, SIF build,
or Tiger action is on the critical path until that vertical slice works.

**Revision 104 input-closure and evidence rule**: T011-A cannot be scheduled until the
candidate exporter output, registered catalogue signature, explicit case
policy, Provider offer-key map, and canonical local ONNX path are all present
in one candidate manifest. The runner must validate their hashes and namespace
bindings before starting NFD/SVS. If any input is missing, the orchestration
records `WAITING_EXTERNAL_INPUT` with its owner and stops. The runner may
retain a precise pre-start cause such as `ENVIRONMENT_MISSING` or
`CANDIDATE_*`; neither is a protocol result. Do not create a temporary
candidate, edit a manifest in place, rebuild SIF, or submit Tiger.

**Revision 104 release-boundary correction (2026-09-03)**: The host release
path now verifies the registered signed QWEN-F manifest, required Qwen3.6-27B
identity and three-stage structure, CUDA execution-provider declaration, and
`cpuFallback=false` before any scheduler call. This closes only the release
validation seam; it does not implement the QWEN-F executable or qualify a
model. T013 therefore remains incomplete, and a fresh T014 convergence audit
is required before SIF promotion.

The coordinator/User lifecycle path now instantiates the guarded writer, binds
the actual request and ACK attempt, and emits each milestone from its
corresponding transition. A live Y-A trace is still required; a post-hoc
synthetic trace cannot close T011 or T014.

**Why work is still incomplete (revision 105)**: implementation and audit
work repeatedly advanced without a valid candidate for the first live request.
This was amplified by three ordering mistakes: T011 combined the driver and
its whole negative matrix, release/SIF work was deferred past the convergence
boundary, and QWEN-F was deferred until after image sealing. The current plan
does not treat those historical refinements as progress toward execution. The
only remaining pre-audit gates are G0 candidate closure, G1 one live Y-A
terminal Response, and G2 Y-B/Y-N plus T013 completion. Missing key/model
artifacts stop at G0; they do not justify another preflight loop.

**Revision 105 task slicing**: For G1, finish only the atomic `FullModel`
path: generic input publication, signed catalogue record, one authenticated
full-model offer, atomic planning/Selection, Provider execution, terminal
Response, and cleanup. Shared-backbone assembly, native Merge, and the Y-N
mutation set are downstream of that result and must not be interleaved with
G1 or used to explain why G1 has not run.

**Iteration 97 launch-path correction (2026-09-03)**: The first live attempt
also requires the maintained Controller child to be invoked in its
Spec180-publication mode. The runner supplies the publication file without a
repository deployment manifest; `controller.py` must therefore select
publication mode from that file alone, publish and read back the signed APP
records, and remain alive for the later User request. A controller that falls
back to ordinary `controller.run()` is a wiring failure, not a protocol
failure. T011-A now includes this command-shape regression before any network
attempt.

**Iteration 98 publication-input correction (2026-09-03)**: Before the
Controller signs any runtime APP Data, the publication batch must pass a
process-boundary decoder that checks candidate case, signer namespace,
manifest/catalogue digests, artifact namespace and uniqueness, bounded payload
size, and per-artifact digests. This is a pre-network mutation gate and is
covered by a focused tamper regression; it does not replace the later exact APP
readback.

**Reason this plan is intentionally narrow**: the generic stream/state
machinery and the Spec175 CPU/MiniNDN baseline are already sealed. The
unfinished work is production integration, not another redesign of NDNSF-DI.
While T011 is open, do not add a planner, placement strategy, transport,
retry policy, model format, or benchmark matrix. A focused repair is allowed
only when the Y-A driver or its production owner exposes a concrete failure;
that repair invalidates later candidate evidence and returns to its focused
test.

## Active-work rule (2026-09-03, revision 105)

Only the smallest real Y-A developer slice is active until it returns a
terminal Response. Do not start another audit iteration, complete-suite run,
SIF build, upload, or Tiger job while T011-A lacks a sealed candidate and a
live terminal Response. The Y-A slice must use the existing
MiniNDN/NFD/SVS helpers, real child processes, the runner-owned signed
catalogue publication/readback, the maintained User's encrypted input
reference, and the actual coordinator request/attempt IDs. A fake publisher,
offline snapshot, legacy deployment-first runner, or synthetic terminal marker
can validate a unit seam but cannot close T011-A. After Y-A passes, extend the
same driver to Y-B and the fixed Y-N controls; do not create a second harness.

This rule separates four evidence levels: `implemented` (source exists),
`wired` (the production caller reaches it), `executed` (the real MiniNDN path
runs), and `qualified` (the task's complete oracle and cleanup evidence pass).
The current Spec180 work has focused `implemented`/`wired` fragments only;
T011-A is the first task that can produce `executed` evidence. Fail-closed
input rejection is expected and is not itself a protocol failure.

Implementation proceeds in three pre-audit slices:

1. **Atomic YOLO slice**: finish the atomic portions of T002/T004--T007/T009/
   T010 and T011; run one non-qualifying Y-A developer MiniNDN request.
2. **Shared/negative YOLO slice**: finish T007--T010 and extend the identical
   driver to Y-B plus Y-N; run one non-qualifying Y-B developer request and
   the smallest focused negative reproducer for each boundary.
3. **Promotion closure**: finish T013, including all SIF/release/preflight
   implementation and the separate Qwen3.6-27B ONNX `QWEN-F` executable.
   Nothing after T014 may add code or change a recipe/profile.

**Single-pass rule**: before T014, run only the focused regression for the
current failing seam and the smallest developer MiniNDN request needed to
observe it. After T014, execute each registered local/SIF/Tiger gate once from
the frozen candidate. A missing protocol result is fixed by implementing the
next ordered vertical slice, not by repeating a complete suite or rebuilding
an image.

Then run T014 once. T015--T019 consume the frozen candidate in order and are
execution-only. Any behavior-affecting change returns to its pre-T014 owner,
invalidates the audit, and restarts at T014 rather than being patched during a
SIF or Tiger run.

**Current implementation boundary (audit iteration 103; documentation revision 108)**: T001, T003, and T012
are complete. The generic request/reference guards, signed catalogue resolver,
candidate-local planning checks, role-ownership guards, role-kind checks, and
the machine-readable tiny Qwen reference manifest have focused evidence only.
The canonical APP owner now publishes and binds `REPO_REF` input through the
native encrypted publisher and records non-secret `INPUT_REFERENCE_PUBLISHED`
metadata; the maintained YOLO helper consumes that path and the native Python
binding exposes its source-bound metadata.
T002 and T004--T010 remain partial; T011 and T013 have focused
release/preflight slices but remain partial; T014--T020 remain unstarted. In particular,
no live NDN catalogue publication,
ACK-to-Selection-to-Provider-to-Response execution, native YOLO Merge run,
MiniNDN qualification, SIF replay, or Tiger job is implied by this plan. The
first formal gate is T014 convergence `PASS`; the source-bound inventory
generator and isolated local-gate supervisor now exist with focused mutation
tests, but the release/profile shell remains a fail-closed dispatch boundary
until the real case runner and candidate-bound inventory are materialized and
exercised, so it cannot produce qualification evidence yet. The registered
YOLO entrypoint now provides the source-bound preflight, candidate case-plan
descriptor, lifecycle-journal transition wiring, barriered phase startup, and
controller-owned catalogue publication/readback; it remains unqualified until
T011 produces a bound lifecycle trace and live terminal Response.
The T011-owned in-image dispatcher now implements that closed candidate-bound
workload contract: it verifies the exact gate/case, raw workload digest,
`/bundle`-relative maintained entrypoint, fixed arguments, allow-listed
environment, fixed image mounts, and fresh evidence root before `exec`.
Fifteen focused dispatcher tests cover positive and mutation paths. The shim
is an execution boundary, not protocol logic or qualification evidence; the
ACK-driven driver is implemented at the launch boundary but has no live
evidence yet. QWEN-F is reserved for the separate Qwen3.6-27B ONNX entrypoint
and cannot dispatch through the Spec175 M11 tiny-model wrapper. The entrypoint
now exists and fails closed until its registered production manifest, staged
object set, and two-request runtime evidence are available; this is an input
boundary, not a qualification result.

The iteration-88 publication correction adds the runner-owned
`publish_and_verify_runtime_catalogue` seam. It does not count a publish call
as evidence: the exact signed APP Data name is read back with the expected
signer and byte-compared before `mark_catalogue_published` unlocks User. The
production driver still must instantiate the controller-node `ServiceUser`,
publish candidate-bound role/rank objects, and invoke this seam.

The iteration-89 cleanup correction adds an instance-owned, idempotent
`MiniNdnCaseRuntime.stop()` boundary. It stops only the children launched by
that runtime, then the case network, invokes the maintained MiniNDN cleanup,
and clears phase/publication state. T011 must call it from `finally` on every
case path; cleanup failure remains an unqualified result. This is an adapter
boundary correction only and does not advance the live driver or T014.

The iteration-90 cleanup correction makes the teardown one-shot: it records
completion before invoking cleanup, turns repeated `stop()` calls into no-ops,
and rejects a later network restart. This prevents a duplicate error handler
from reaching global MiniNDN cleanup after another case starts. The correction
is covered by the focused runner test and does not advance T011 or T014.

The iteration-91 integrity correction closes two adapter acceptance gaps. The
runtime catalogue payload builder now requires one-and-only-one snapshot per
registered candidate and exact coverage of that candidate's complete case
role set with unique absolute artifact Data names. The signed APP readback
path also requires a non-empty certificate rooted at the configured
controller signer. These checks prevent incomplete or unverifiable snapshots
from being treated as ready, but they do not wire the production publisher or
advance T011/T014.

The iteration-87 parser correction additionally rejects non-`ACTIVE` snapshots
at the signed runtime snapshot parse boundary. Retired or revoked catalog
objects remain historical metadata only and cannot enter request-scoped
placement. This is a parse-boundary guard; publication and the live ACK-driven
driver remain open.

The iteration-86 parser correction additionally rejects duplicate candidate
digests in the signed runtime snapshot, matching the one-ACTIVE-record-per-
candidate contract. This is a parse-boundary guard only; publication and the
live ACK-driven driver remain open.

The iteration-85 audit additionally repaired the V3 artifact boundary. When
the maintained path supplies an explicit signed catalog resolver,
`AutomaticPlanningCoordinator` now calls the existing `_prepare_artifacts`
authority before sealing Selection, and a focused regression proves exact
role/rank resolution from a matching ACTIVE snapshot. The repair intentionally
does not invent a publisher: the runner still lacks the candidate-bound
pre-split role objects and active runtime snapshot required by the User.
The offline/package `spec180-yolo-catalogue-v1` record is therefore not a
substitute for the runtime `ndnsf-di-presplit-catalog-snapshot-v1` APP record.

The iteration-84 audit (now superseded by iteration 85) additionally binds the catalogue APP record to the
native naming rule: its exact Data name is below
`/<controller>/NDNSF/DI/`, and its expected signer is the same controller
identity. The runner rejects free-standing catalogue names before startup.
The case adapter's keychain identity lookup and missing-provider-prefix error
path are covered by focused regressions. The current runner slice has 43
passing tests and the complete Spec180 Python collection has 169 passing with
22 existing warnings; these remain implementation evidence only.

The iteration-76 audit (historical snapshot; superseded by iteration 78) also corrected two evidence-authority drifts. The local
Q-C/Q-W inventory now derives the wrapper's M01/M11 and seed arguments from the
frozen Qwen reference manifest (`175021` and `175022`), rather than duplicating
the obsolete `1750001` seed. The terminal-result validator now verifies the
semantic two-request, three-CUDA-device, no-CPU-fallback, unique-child, and
cleanup assertions. That checkpoint recorded 143 passing Spec180 Python tests
with 22 warnings; the current collection is recorded below and no
qualification status changes.

The iteration-77 design audit tightened the fixed `Y-N-C` negative so it
removes `FullModel` and at least one required shared-candidate capability;
therefore neither registered candidate can remain feasible. It also makes the
real MiniNDN implementation reuse the existing startup, routing, keychain,
and process-supervision helpers from `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`.
The Spec180 runner may adapt candidate-specific inputs and commands, but must
not fork a second NFD/SVS/security harness or invoke the legacy deployment-
first `main()` as an oracle. This is a design correction only; T011/T014 and
all formal qualification gates remain open.

The iteration-78 profile audit closes a second setup ambiguity. The registered
runner now requires exactly one Provider identity for Y-A and exactly four for
Y-B/Y-N, with a bipartite distinct-capability cover for every shared role (and
an additional `FullModel` capability for Y-N). This is only a preflight
capability witness; it is not a role assignment and cannot replace the live ACK
snapshot. The iteration-79 lineage correction makes the lifecycle identity
protocol-bound: the live driver must bind its coordinator `requestId` and ACK
attempt identity before the first event. Provisional IDs are test-only and
cannot qualify a case. Focused runner tests pass; the live NFD/SVS driver,
T014 convergence, and all qualification gates remain open.

Iteration 68 corrects the pre-start policy boundary in the registered runner:
case-specific role sets, per-Provider authorized capability advertisements, and
the controller/user/repository/Provider node map must be present in the
candidate config and resolve to nodes in the supplied topology. A role may be
advertised by multiple Providers and a Provider may advertise multiple roles
when the fixed case has a distinct capability cover; these entries authorize
capability advertisement only. ACK closure and the sealed plan remain the sole
runtime placement authority, with one Provider per complete selected role.
The focused runner collection is now 37 passing tests. The live NFD/SVS
driver and candidate-bound inventory execution remain open.

An earlier checkpoint reran the complete Spec180 Python collection after the
dispatcher, inventory, result-validator, fixed-profile, and lifecycle-binding
changes: 145 passed with 22 exporter/runtime warnings. This historical count is
superseded by iteration 83 and remains focused source evidence only; no
MiniNDN, SIF, or Tiger qualification is inferred. The release validator
consumes the dispatcher schema before submission, and the remote wrapper
rechecks mounted model/workload digests before output-directory creation; both
remain fail-closed boundary checks.

The runner also materializes a fresh case-local `case-policy.json` from the
candidate config and binds its digest in `case-input.json`. It filters only the
registered case roles and authorized capability advertisements, without
mutating the source policy or making a request-time placement choice. The live
NFD/NDN-SVS driver remains intentionally fail-closed until T011 is complete.

The iteration-81 boundary additionally invokes the maintained `policy.py`
parser and compatibility checks on both the source policy and the isolated
case policy before network creation. This is a preflight consistency check,
not policy generation or certificate installation; the later live driver still
owns those side effects. The focused runner slice is 28 passing tests and the
complete Spec180 Python collection is 153 passing with 22 existing
exporter/runtime warnings. Neither count is MiniNDN, SIF, or Tiger evidence.

The historical iteration-79 audit also makes lifecycle evidence
protocol-bound: the live
driver must pass its coordinator `requestId` and ACK attempt identity to the
journal before the first event. Automatically generated IDs remain a test-only
fixture convenience and cannot qualify a case.

The iteration-67/68 audit records are historical. Iteration 68 corrected its startup
policy terminology and validation: `service.providers[*].roles` is an
authorization/capability allowlist, not a fixed owner map; overlap is allowed,
and final assignment remains ACK/sealed-plan owned. That historical
runner/local-gate/inventory/contract slice had 37 passing tests and the
historical Spec180 collection had 122 passing tests with 19 existing exporter/runtime
warnings. These are focused/document gates only. The real NFD/NDN-SVS driver
remains unwired and formal validation is still blocked at T011/T014.

The historical iteration-69 audit added four implementation corrections and one wording
clarification. Safe cuts had to be
proved from YOLO branch/tensor semantics and role interfaces; node-count
percentiles or topological indexes were not certification. At that checkpoint
the remote shell probed an in-image `scripts/run_spec180_case.py` that was not
yet in the repository, so T011 owned a thin, candidate-bound dispatch shim and
the remote job failed closed until it existed. Terminal result oracles had to be
structured evidence references with paths, digests, and schemas rather than
bare `PASS` labels. Lifecycle events use an allowlisted field schema and
recursive redaction, not a substring check alone. These are documentation and
acceptance corrections; the real NFD/SVS driver remains unwired and no formal
validation is implied.

The iteration-70 checkpoint implements the lifecycle correction: the runner's
journal now applies the closed milestone field allowlist and scalar-only value
rule, with focused negatives for generic payloads, unknown fields, nested
objects, and secret-bearing values. This remains evidence hardening only; the
real driver and convergence gate are still required. The terminal validator
also now rejects label-only oracle values and verifies structured oracle files
against an explicit evidence root; wiring the future result writer to emit
those records remains T013/T020 work.

The iteration-71 checkpoint replaces the YOLO percentage/index partition with
a graph-bound semantic certificate. Export records architecture-scope node
sets, lifted-constant ownership, crossing tensor contracts, dependency edges,
semantic safe cuts, and the full-model equivalence oracle; the adapter rechecks
all of them against the loaded graph. This closes the heuristic-cut gap only;
native assembly, live ACK-driven execution, and qualification remain gated by
T007--T014.

The historical iteration-50 audit reran the strict structure and contract
gates and a then-current focused collection (105 passed, 19 warnings). It corrected stale
iteration-32/42 status language and made the candidate-bound Provider-offer
trust policy a separate versioned contract. These checks do not implement the
production verifier or the live Y-A/Y-B/Y-N driver; formal validation remains
blocked until those paths and the T014 convergence audit pass.

Iteration 51 adds the reusable `ProviderOfferTrustVerifier` and an ACK-aware
factory integration test. It closes only the local policy/signature/provenance
implementation boundary; the real Trust Schema certificate callback, maintained
ACK-to-Response runner, and qualification evidence remain required.

Iteration 52 carries an explicit `trustSchemaValidated` bit from the native
packet-backed ACK projection through pybind/Python. The maintained YOLO example
now constructs the Provider-offer verifier from the candidate-bound policy and
PEM key map and rejects the historical HMAC option. This closes a source-level
trust-boundary contradiction only; it does not replace native certificate-chain
execution or the missing Y-A/Y-B/Y-N runner.

Iteration 53 corrects the native subscription wiring: the ACK regex subscription
now retains the validated Data packet instead of using the payload-only mode.
The resulting signer/KeyLocator/wire-digest projection can therefore be
populated on the real `ServiceUser` path; the focused wiring test and native
rebuild do not count as a live ACK-to-Response or qualification run.

Iteration 54 freezes the Spec175 receiving boundary. Spec180 records the
post-handoff source delta as its own subject and must rerun T014 convergence;
the current-tree source seal is a byte-integrity check only and cannot turn the
frozen Spec175 local results into current Spec180 evidence. The plan also keeps
the YOLO one-shot oracle separate from the Qwen prefill/decode stream oracle.

Iteration 55 makes the Y-N runner executable rather than descriptive: one fixed
catalogue-order invariance control and six fixed fail-closed negatives run in
fresh subcases under the single `Y-N` inventory entry. Every lifecycle event is
emitted once as a non-secret JSONL record with request/attempt and
candidate/plan digests where applicable; duplicate, missing, or out-of-order
events invalidate the case. This is an execution/evidence clarification, not
an additional placement strategy or qualification result.

The release boundary was corrected in iteration 39: the renderer now serializes
the allow-listed `SPEC180_*` values as `--export=NONE,<explicit assignments>`
so Slurm does not drop the run identity, and the checked-in job wrapper verifies
the fixed Apptainer/SIF before probing and entering the case runner inside the
image. The inventory generator and isolated local-gate supervisor now exist,
but candidate-bound materialization and the real case driver remain
unimplemented; the iteration-42 inventory wording now requires one supervised
execution per inventory item, with no duplicate formal-case pass. This
correction improves launch correctness and evidence isolation but does not
authorize any qualification execution. Iteration 43 adds the explicit
candidate-policy/trust boundary documented below.

The iteration-43 runner audit (historical; superseded by iteration 68) resolved
a policy ambiguity: the checked-in
`yolo_2x2/yolo_policy.yaml` is a legacy `/Stage/...` deployment-first policy
and cannot be used for Y-A/Y-B/Y-N. T011 must materialize an isolated policy or
runtime role registration from the signed candidate catalogue and selected
plan, with `FullModel` capability authorization for Y-A and four distinct
component-role capability advertisements for Y-B. The runner contract requires explicit candidate-bound paths
and a production trust-backed offer verifier before MiniNDN startup; the
existing caller HMAC map remains fixture-only.

Iteration 44 (historical; superseded by iteration 45) closed an additional design/code mismatch before any
qualification run: the YOLO exporter emits external initializer bytes
separately from the graph, while the prior native assembler staged only the
graph. The assembler contract now binds graph and initializer objects
independently (name, size, and raw digest), stages `model.onnx.data` through
the native helper, and checks the recipe's normalized initializer digest after
loading. The focused regression passed; the real signed package and
ACK-driven runner remained open. The publication-side gap in this historical
description was closed by iteration 45 and is not a current implementation
condition.

Iteration 45 closed the publication half of that boundary: the canonical root
API carries the initializer name/size/raw digest, and the catalog ensurer
publishes and verifies the initializer payload before the root-last sequence.
The corresponding canonical-layer regression passes. Signed package creation,
production ACK wiring, and the real YOLO runner remain open.

Iteration 46 (historical; superseded by iteration 47) made the ACK-offer trust boundary explicit: formal Y-A/Y-B/Y-N
cases use the existing NDNSF Trust Schema and Provider identity/certificate
verification anchored by candidate-bound `SPEC180_YOLO_OFFER_TRUST_ROOT`.
Verification binds the signer to the advertised Provider/service and the
canonical `ProviderOfferV3` digest, request/attempt, model/graph, validity
window, and boot epoch. The caller HMAC key map remains fixture-only. The
verifier wiring and live ACK-to-Response execution are still open under
T006/T009/T011.

Iteration 47 records the concrete production wiring prerequisite found by the
audit: the NDN ACK Data is Trust-Schema validated before decryption, but the
current C++/pybind `AckCandidate` projection drops the validated packet's
signer/key-locator provenance and wire digest. T006 therefore owns the narrow
ServiceUser/pybind projection change that carries this non-secret provenance
into the Python candidate, and the V3 verifier must reject missing or
mismatched provenance before feasibility. This reuses the existing Trust
Schema validation result; it does not create a second key map or make the
caller HMAC fixture a production authority.

Iteration 48 implements that projection through `ServiceUser`, pybind, and the
Python `AckCandidate` facade, with a focused preservation/legacy-default
regression. The implementation still requires a native build/import check and
the production ProviderOfferV3 verifier plus live negative/positive path
before T006 or convergence can close.

Iteration 49 also closes the configuration-side ACK-window contradiction:
`APPClient.configure_automatic_planning()` rejects caller-supplied
`ack_coverage_roles` and `ack_coverage_predicate` for V3, so the registered
ACK timeout remains the only discovery-closure authority. The model-neutral
`SequentialFixtureSplitter` also declares its first and last roles as the
explicit ingress/egress owners required by V3 validation; this is only a
Spec170 compatibility repair and is not the Spec180 YOLO adapter. This does
not replace the required lower-level/live-path check.

## Technical Context

**Language/Version**: C++17; Python 3.10 inside the deployed SIF; current host
Python for host tools only; Bash and Slurm for deployment.

**Primary Dependencies**: ndn-cxx; NDN-SVS `Experimental` with Boost 1.71;
NAC-ABE; NFD; pybind11; ONNX and ONNX Runtime CPU/CUDA; standalone tokenizer;
Apptainer 1.5.3 and Slurm. Ultralytics/PyTorch may exist only in the offline
YOLO export environment.

**Storage**: Content-addressed canonical YOLO model objects plus the encrypted
registered YOLO invocation input outside the SIF; Provider-local assembled-role
caches; signed JSON/wire manifests;
candidate-bound JSON/JSONL evidence that excludes input/result plaintext.

**Testing**: Focused pytest and Boost.Test; C++ integration; deterministic ONNX
numerical oracle; CPU/MiniNDN production-path cases; exact-SIF local replay;
one finite Tiger functional job.

**Target Platform**: Linux development host; CPU MiniNDN; locally built
immutable Apptainer SIF; one-node UofM TigerCluster RTX 6000 allocations.

**Project Type**: C++ framework plus pybind11/Python application SDK, model
adapters, MiniNDN harness, packaging, and Slurm operation.

**Performance Goals**: None. Elapsed time is diagnostic only; no throughput,
latency, scaling, or comparative performance claim is permitted.

**Constraints**: One Provider owns one complete role; one Provider is not reused
for another role in one plan; `PreSplitFirstStrategy` remains default; no
caller-supplied Provider list or role map in the qualification path; no
cross-Provider tensor parallelism; no runtime PyTorch/Transformers/Ultralytics;
no CPU model-compute fallback in the Tiger functional job (CPU is allowed and
reported for local cases); one SIF and one Tiger job, with at most one
byte-identical pre-workload infrastructure resubmission authorized by contract.

**Scale/Scope**: YOLO26n atomic and four-role shared-backbone two-shard
candidates; one cold Tiger request; one allocated GPU shared by the three
independent model-role Provider processes; a fourth Provider performs declared
CPU merge/postprocessing rather than model inference.

## Constitution Check

### Pre-design gate

| Principle | Result | Design consequence |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Extend the current model-first DI facade and coordinator; do not add a YOLO service protocol or generated stub. |
| Security Is Part Of The Data Path | PASS | ACKs, plans, assignments, canonical objects, invocation-input references, dependencies, events, and Responses retain current authentication, authorization, confidentiality, token, replay, and epoch checks. |
| CodeGraph First | PASS | Existing coordinator, facade, adapter, assembler, native Provider, YOLO example, Qwen path, and tests were inspected before planning. |
| Spec-Driven Durable Work | PASS | Spec175 handoff, this plan, contracts, tasks, audit, and immutable evidence define the work. |
| Verify With The Right Scope | PASS | Focused tests precede convergence; complete suites/MiniNDN follow audit PASS; SIF/Tiger are last. |
| Cohesive Tasks | PASS | Each task combines contract, failing test, implementation, focused validation, and evidence for one behavior. |
| Immutable Promotion | PASS | One candidate binds source, SIF, harness, profiles, models, workloads, submit bundle, and evidence; rejected mutations have zero external side effects. |
| Design-Code Convergence | PASS | A mandatory post-implementation audit blocks all formal local/SIF/Tiger qualification until production wiring agrees. |

No constitution exception is required.

### Post-design gate

PASS. The design reuses one planner and one native runtime, keeps model-specific
semantics in adapters, defines fail-closed security and role ownership, and
separates functional qualification from performance research.

## Architecture Decisions

### 1. Reuse one request-first planner

`AutomaticPlanningCoordinator.request()` remains the only owner of this order:

```text
INPUT_REFERENCE_PUBLISHED (REPO_REF only)
  ->
REQUEST_SENT
  -> ACK_CLOSED
  -> GRAPH_READY and certified candidate enumeration
  -> PLACEMENT_DECISION
  -> ARTIFACTS_READY
  -> PLAN_SEALED
  -> SELECTION_COMMITTED
  -> Provider execution
  -> terminal result
```

Before `ACK_CLOSED`, the coordinator may verify the signed catalogue revision
and trust-root binding as an opaque static input-safety check. It must not expose
candidate records or requirements to placement, evaluate feasibility, or bind a
Provider until the authenticated ACK snapshot is closed. This distinction keeps
catalogue integrity validation separate from request-scoped candidate choice.

The preflight may retain an immutable catalogue revision, signature, and digest
in a case descriptor for reproducibility. That metadata is opaque safety input
only. Candidate records, role requirements, feasibility, and Provider binding
remain unavailable to the planner until the authenticated ACK snapshot closes.

The live capability authority is the immutable `CollaborationAckClosed` value
returned by the existing collaboration path. `PreSplitCatalogSnapshot` is a
different object: it names already-published canonical role artifacts for the
selected candidate. A snapshot resolver may read it only from an authenticated
repository/data path and must verify its manifest, candidate, model, graph, and
artifact digests. It must never synthesize ACKs, close the ACK window, choose a
candidate, or replace the checked-in catalogue trust root. A file-backed
snapshot or caller-provided Provider-key map is permitted only in focused
offline fixtures.

Spec180 does not add a YOLO planner. It generalizes the model-first request
boundary so generic `InferenceTaskRef`, `ApplicationInput`, and `TaskOptions` can reach
the existing coordinator. The current Qwen `GenerationInput` and
`GenerationConfig` path remains a typed convenience adapter over the same
coordinator. The old deployment-first `distributed_inference()` path remains
deprecated compatibility and cannot produce Spec180 evidence.

The generic YOLO entry point is the existing
`InferenceClient.request_task(model=..., task=..., input=..., timeout_ms=...,
options=..., strategy=...)`; `InferenceApplication.request(...)` delegates to
that path when given `ApplicationInput` plus `task`/`task_options`.
`InferenceClient.request_model()` remains limited to
`GenerationInput`/`GenerationConfig` and is not a generic object-detection
entry point. Documentation and examples must use these actual signatures.

For the registered YOLO `REPO_REF` case, the maintained caller first uses the
canonical `app_sdk.client.APPClient.publish_application_input_reference()`
method. That method delegates publication to the native encrypted publisher,
binds the source bytes to the native name/object ID/size/content digest,
authorization scope, protection epoch, encryption bit, and canonical
publication-manifest digest, then records non-secret
`INPUT_REFERENCE_PUBLISHED` metadata. The complete bound reference is passed
to `ApplicationInput.from_repo_ref()` with a durable publication digest;
`APPClient.request_task()` rejects a missing or altered journal binding before
publishing `REQUEST_SENT`. The lower network facade's legacy
`publish_large_payload_reference()` remains a compatibility/offline helper and
does not by itself satisfy this contract. Merely loading a JSON reference from
`--input-reference-file` is offline-fixture input only. The selected ingress
Provider remains the only role that fetches/decrypts plaintext after Selection.

The legacy V2 compatibility path still carries a `required_roles` field, but
the coordinator rejects a heterogeneous candidate catalogue before that field
can influence a decision. The V3 path evaluates each candidate against its own
role/dependency requirements and orders feasible candidates by the
adapter-signed priority followed by candidate digest. The generic planner must
not contain YOLO-specific role names.

Because the candidate catalogue is enumerated only after `ACK_CLOSED`, the
Spec180 qualification coordinator also leaves `ack_coverage_roles` and any
offline role-coverage predicate empty. The registered 1500 ms ACK window owns
closure. Candidate-specific role coverage is evaluated afterward by placement,
not used to terminate discovery early. Every candidate also names its single
input-ingress and result-egress role so input fetch and terminal publication
cannot be inferred from catalogue order. A Spec180 caller that supplies a
  non-empty `ack_coverage_roles` or equivalent offline role predicate MUST be
  rejected for this qualification path; it must not be silently ignored.

The production Provider context receives those identities through the V3
`RoleDataflowContract`: it contains one `APPLICATION_INPUT` endpoint consumed
only by the declared ingress role and exactly one terminal-response owner equal
to the declared egress role. Non-owner attempts fail closed with the stable
structured reasons `DI_INPUT_FETCH_ROLE_MISMATCH` and
`DI_TERMINAL_RESPONSE_ROLE_MISMATCH`.

When multiple registered candidates are feasible, the YOLO adapter supplies a
signed priority in its canonical catalogue. The placement strategy evaluates
all feasible candidates, then applies that priority and finally a stable
candidate-digest tie-break; it never uses catalogue position. The registered
shared-backbone candidate has higher priority than the atomic candidate, but it
is selected only when all four role requirements are met.

### 2. Make invocation-input transport explicit

The public API reuses `ApplicationInput`, but its wire form has two mutually
exclusive modes:

```text
INLINE: payload bytes, size <= 4096 bytes
REPO_REF: authenticated LargeDataReference; no payload bytes in the Request
```

The registered YOLO case always uses `REPO_REF`. Before publishing the generic
Request, the User stores the input through the existing repository large-object
path under the request security policy and records
`INPUT_REFERENCE_PUBLISHED`. The Request binds the NDN name, input schema,
plaintext size, content digest, encryption flag, and protection epoch. The
current legacy wire key `ciphertextDigest` is a compatibility alias for the
post-decryption plaintext digest, not an encrypted segment-wire digest.
Candidate Providers can decide whether to ACK without reading the image.
After Selection, only the candidate-declared input-ingress role receives an
assignment-scoped authorization/decryption capability and may fetch, decrypt,
and verify the object; knowing the NDN name alone MUST NOT grant access. The
Provider helper must call the existing reference-aware fetch primitive (or a
semantically equivalent primitive) so manifest/content digest and encrypted
authorization are verified before plaintext is returned. Calling a name-only
or size-only `fetch_large` is insufficient and must fail with
`DI_INPUT_REPO_DIGEST_UNVERIFIED`. The candidate-declared result-egress role
alone publishes the terminal result. The input and final result use existing
NDNSF confidentiality and authorization;
plaintext is forbidden in logs and evidence.

This task replaces the current unconditional base64 embedding in
`DIRequestEnvelopeV2`; it does not introduce a second repository protocol.
The backward-compatible encoding keeps `DIRequestEnvelopeV2`: `INLINE` retains
`input_payload_b64`, while `REPO_REF` requires it to be empty and stores the
canonical transport/reference contract under `task.input_transport`;
`input_manifest_digest` covers either form. Qwen's bounded input remains valid
through the inline mode.

The coordinator projects the candidate-declared ingress role into the plan's
dataflow rather than deriving it from role names. The projection contains one
`TensorEndpoint(source_kind=APPLICATION_INPUT, producer_role="", consumer_role=
input_ingress_role)` and exactly one terminal-response owner. This projection
is part of the plan digest and is passed to the Provider runtime, so an
assignment cannot silently move input fetch or result publication.

### 3. Keep YOLO semantics in a real adapter

The current `build_object_detection_adapter()` constructs a synthetic two-node
shape; it does not inspect a real YOLO graph. Spec180 adds a YOLO adapter under
`adapters/yolo/` that validates the canonical graph, tensor interfaces,
preprocessing/postprocessing identity, initializer ownership, and safe cuts.
It emits exactly two candidate families for this feature:

```text
Atomic:
  FullModel

SharedBackbone:
  BackboneNeck -> DetectShard0 --\
                -> DetectShard1 ----> Merge
```

The exact head-output names and initializer ranges come from the canonical
manifest; the abstract role names above are stable. A candidate is accepted
only when its signed descriptor contains semantic node sets, branch ownership,
producer/consumer tensor interfaces, dependency edges, and a full-model
equivalence check for those cuts. A 55% prefix, fixed node index, or other
topological-position heuristic is not a safe-cut proof. `Merge` is a complete role
with declared postprocessing and may execute on CPU. Replicated-backbone
variants, arbitrary graph cuts, N-by-M layouts, and tensor parallelism are not
silently accepted.

### 4. Offline export prepares choices, not the final plan

The offline tool may load `yolo26n.pt`, export and normalize canonical ONNX,
externalize weights, derive graph/tensor metadata, and certify safe candidate
descriptions. It has no live Provider ACKs and therefore cannot bind Provider
identities or choose the request's final candidate. The exported package and
oracle input/output are immutable and content-addressed. Graph and external
initializer objects are separate authenticated package entries; the native
Provider fetches and verifies both before calling ONNX Runtime, while the
recipe's normalized initializer digest is checked after loading them together.
Each candidate records
an explicit input-ingress role and result-egress role; the shared candidate
uses `BackboneNeck` and `Merge`, while the atomic candidate uses `FullModel` for
both.

The catalogue trust root is owned by the versioned
`contracts/catalogue-trust-root-v1.md` contract plus its registered entry in
`contracts/trust-root-registry-v1.json`, with T001 owning the registration.
Export and runtime code may consume those records, but neither a Provider ACK
nor a Tiger profile may replace their signer identity or covered-field set. An
unconfigured registry is a deliberate implementation blocker, not a usable
default.

Provider-offer verification has a separate candidate-bound policy contract in
`contracts/provider-offer-trust-v1.md`. This policy restricts accepted
Provider/service/certificate identities while delegating packet and certificate
authentication to the existing NDNSF Trust Schema. It is not a second key map,
and its presence does not imply that the production verifier is wired.

### 5. Provider assembly is the production execution boundary

The existing `RoleAssemblySpec`, canonical publication, and
`prepareNativeCanonicalOnnxRole()` path are reused. It is extended only where a
branched YOLO role needs multiple declared inputs or outputs. The `Merge` role
is a native dependency-consumer and canonical postprocessing owner. The default
certified YOLO merge is `NATIVE_POSTPROCESS` and has no model-layer objects;
ONNX Runtime CPU is permitted only when the manifest explicitly declares an
`ONNX_MERGE_GRAPH` and its digest. It MUST NOT execute or silently host
model-layer computation that belongs to a GPU role. Each selected
Provider verifies the signed request/ACK/model/candidate/plan/role/Provider
binding and canonical object digests before assembly or cache reuse. The
qualification path rejects the source-compatible `plaintext-v1`
`RoleAssemblySpec` default and requires a protected authorization epoch. Python may
prepare metadata, but deployed model execution occurs through the native ONNX
Runtime Provider path.

`RoleAssemblySpec` currently allows `COMPONENT_SET` in its enum but still
rejects any role without a positive Transformer-style layer interval. T007
replaces this mixed rule with role-kind-specific validation: pipeline/rank roles
use layer ranges, while YOLO component roles use an exact canonical node set and
the reserved wire sentinel `layer_begin=0, layer_end=0`.
The same invariant is enforced by Python serialization, native parsing,
bindings, assembly, and tests. Cache identity additionally binds the security
domain and protection epoch; cache reuse never bypasses current-request
authorization.

### Ownership matrix

| Concern | Single production owner | Spec180 extension | Acceptance owner |
|---|---|---|---|
| Request/ACK closure and candidate feasibility | `AutomaticPlanningCoordinator` | candidate-local requirements and signed catalogue verification | T006 |
| Generic input transport | application SDK/request encoder | `INLINE`/`REPO_REF` wire view | T002/T009 |
| YOLO graph and candidate semantics | YOLO adapter/exporter | canonical graph, safe cuts, merge contract | T004/T005/T008 |
| Catalogue trust root and signature verification | T001 registry/contract gate plus YOLO adapter | configured public-key identity and covered-field canonicalization | T001/T004/T005 |
| External model-manifest trust | T001 registry/contract gate plus release validator | registered artifact authority and signed YOLO manifest fields | T001/T004/T017 |
| Provider-offer trust | existing NDNSF Trust Schema/identity verifier | candidate-bound offer-trust anchor, authenticated ACK signer provenance, Provider/service binding, canonical offer digest and validity checks | T006/T011 |
| Role assembly and execution | native Provider/assembler | component-set and dependency-consumer merge | T007/T008 |
| Deferred Qwen generation/state | existing Spec175 coordinator and adapter | preserve the handoff; no Spec180 execution claim | T012/T019 |
| Local/remote qualification | registered harness/release tooling | candidate-bound manifests and closure | T013--T020 |

No task may introduce a second owner for a row in this matrix; changes to an
owner's production path invalidate the convergence audit.

**Provider-boundary clarification (2026-09-04)**: Spec180's process vector
starts native Providers only. The Python Provider remains an alternative
compatibility/application implementation. Its public `app_sdk.APPProvider`
delegates network serving to `app_sdk.facades.APPProvider` within one Python
process. Native canonical assembly invokes the existing Python model-format
helper; that helper is not another network Provider or ONNX executor.
For Python V3 serving, a required or explicitly declared V3 Selection must
pass the centralized `ProviderSelectionProjectionV3` validator before artifact
preparation; rejection cannot fall through to legacy execution. Preparation
snapshots inherit the validated Selection's attempt. Their initial status
epoch is 1 within the Core's Selection-digest-scoped operation and is not a
Provider boot epoch. Legacy non-V3 callers keep their existing entry points.
T002/T006/T007 own this correction and its handler-level regressions in
`tests/python/test_ndnsf_di_provider_v3_boundary.py`.

### 6. Qwen is a deferred compatibility boundary

Spec180 does not redesign or execute prefill, decode, token streaming,
KV/recurrent state, or conversation semantics. It retains the Spec175 handoff
for compatibility. Qwen3.6-27B packaging and Tiger execution require a later
feature and cannot block or satisfy the Spec180 verdict.

### 7. Functional qualification uses finite cases

Local formal cases:

- Y-A: atomic-only ACK capability profile;
- Y-B: four-role shared-backbone two-shard capability profile;
- Y-N: one catalogue-order invariance control plus the fixed capability,
  provenance, role-kind, non-ingress, protection-epoch, and redaction negatives
  defined by the runner contract;
- Q-C: inherited cold streamed generation;
- Q-W: inherited same-conversation continuation and mismatch rejection.

Q-C and Q-W are historical handoff references, not Spec180 formal cases.

Tiger uses exactly one job from one SIF candidate:

- `yolo-functional`: one node, one RTX GPU, four Provider processes; the three
  model roles all bind CUDA device 0 and `Merge` is an explicit CPU Provider;
  one cold request; 1500 ms ACK timeout, 60000 ms request timeout, and a
  5000 ms initial SVS settle.

The job proves a functional multi-Provider deployment. It is not a benchmark
sample and does not prove physical multi-GPU distribution.

## Pre-Qualification Design-Code Convergence

**Design authority**: `spec.md`, this plan, Spec175
`handoff-to-spec180.md`, all Spec180 contracts, the one-Provider/one-complete-
role invariant, and the registered workload/oracle manifests.

**Production paths to inspect**:

- high-level request facade, inline/reference input encoder, repository-backed
  selected-role fetch, and `AutomaticPlanningCoordinator`;
- adapter registry, real YOLO graph/candidate adapter, Qwen adapter;
- canonical publisher/materializer and signed plan/assignment sealing;
- native Provider role preparation, ONNX Runtime execution, dependency I/O,
  merge, streaming, and terminal response;
- NDN authentication/authorization/confidentiality owners, protection-epoch
  checks, replay checks, and plaintext-log exclusions;
- MiniNDN launchers, SIF builder/preflight, Spec180 profile renderer, submit
  entrypoint, result validator, child supervision, and cleanup.

**Required audit artifact**:
`specs/180-ack-driven-cross-model-qualification/audit.md` with exact
requirement-to-code-to-focused-test-to-formal-case traceability.

**Closure rule**: Any unresolved semantic, architecture, security,
production-wiring, role-ownership, numerical-oracle, or evidence-validity gap is
`BLOCK`. Repair it with a focused failing test and focused passing regression,
then rerun the audit to `PASS`.

**Formal validation boundary**: Complete unit/integration suites, formal
MiniNDN, SIF build/replay, upload, staging, Slurm, and Tiger jobs all depend on
audit `PASS` and Spec175 `LOCAL_FUNCTIONAL_PASS`.

**Re-audit triggers**: Behavior-affecting source, design, dependency, adapter,
canonical manifest, effective configuration, harness, profile, submitter,
oracle, or evidence-schema changes.

## Immutable Candidate

The candidate identity is:

```text
source tree + dependency/toolchain lock + native/Python runtime
+ SIF build recipe + local test/MiniNDN/SIF replay harness
+ Spec180 submit bundle + rendered effective profiles
+ YOLO and Qwen canonical artifact manifests
+ registered input references/workloads/oracles + security-policy epochs
+ validation/evidence schema
```

The pre-build closure locks every transitive input and proves mutation
rejections make zero upload, SSH mutation, staging, scheduler, or job calls.
After local construction, the final seal adds the exact SIF SHA-256. Tiger only
verifies, stages to node-local scratch, and executes this hash with external
read-only content-addressed models.

### Change-plane invalidation

| Changed plane | Invalid evidence | Earliest restart |
|---|---|---|
| Spec, contract, role invariant, public or production behavior | audit and all later evidence | affected implementation task, then convergence audit |
| Runtime source or dependency/toolchain | complete local, MiniNDN, SIF, Tiger | convergence audit |
| Test/MiniNDN harness or local oracle | affected local evidence and all promotions | formal local qualification |
| SIF recipe, embedded dependency, native/Python extension | SIF and Tiger | pre-build closure and SIF qualification |
| External YOLO/Qwen artifact or oracle | consuming model gates and Tiger | model package preflight, candidate closure |
| Invocation input, input reference, security policy/key epoch, or plaintext-redaction rule | consuming local/SIF/Tiger gates | focused input/security tests, then formal local qualification |
| Submit wrapper, profile, resource/device map, environment allowlist | remote closure and Tiger | pre-dispatch closure |
| Result parser/evidence schema | affected verdicts | validator regression and earliest consuming gate |
| Slurm/host incident before workload entry with identical bytes | no functional evidence exists | one recorded byte-identical resubmission |

No evidence is spliced across candidates.

## Project Structure

```text
specs/180-ack-driven-cross-model-qualification/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── tasks.md
├── audit.md
├── traceability.md
├── contracts/                         # versioned protocol and release contracts
│   ├── ack-driven-yolo-v1.md
│   ├── catalogue-trust-root-v1.md
│   ├── model-manifest-trust-v1.md
│   ├── trust-root-registry-v1.json
│   ├── provider-offer-trust-v1.md
│   ├── cross-model-qualification-v1.md
│   ├── immutable-candidate-v1.md
│   ├── local-suite-inventory-v1.md
│   ├── qwen-reference-manifest-v1.json
│   ├── qwen-reference-v1.md
│   └── tiger-profile-v1.md
└── evidence/

NDNSF-DistributedInference/
├── ndnsf_distributed_inference/app_sdk/       # generic model/task request
├── ndnsf_distributed_inference/adapters/yolo/ # real YOLO adapter
├── ndnsf_distributed_inference/adapters/qwen/ # frozen reference adapter
└── cpp/ndnsf-di/                              # native assembly/runtime

examples/python/NDNSF-DistributedInference/yolo_2x2/
Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
packaging/ndnsf-di-container/jobs/spec180/
scripts/spec180_*.py
scripts/run_spec180_local_gate.py
scripts/run_spec180_case.py                 # thin in-image dispatcher
tests/unit-tests/
tests/integration-tests/
tests/python/
```

**Structure Decision**: General request orchestration and native execution stay
model-neutral. YOLO graph/split/output semantics live only in the YOLO adapter
and offline exporter. Qwen remains in its adapter. Experiment and deployment
policy remain outside the runtime.

## Delivery Phases

1. **Native closure and atomic vertical path**: First prove one resolved,
   hashed `libndn-cxx` closure for NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python
   extension. Then finish only the generic/adapter/runtime pieces needed for
   one real Y-A request, then execute one developer MiniNDN request
   through ACK closure and terminal Response.
2. **Shared and negative path**: Extend that same driver to Y-B native Merge
   and the fixed Y-N controls; do not create another harness.
3. **Pre-audit promotion closure**: Finish the YOLO release/SIF/preflight/result
   tooling under T013.
4. **Freeze and qualify locally**: Run one T014 convergence audit, then one
   complete T015 local campaign from the unchanged candidate.
5. **Immutable SIF**: Build one local SIF and replay Y-B.
6. **Tiger qualification**: Stage accepted bytes once, run one YOLO-F request,
   and issue one functional verdict without modifying the candidate.

### T011 implementation boundary (iteration 92)

T011 is one behavior task with four ordered implementation slices; the slices
are not separate qualification cases:

Apply the four technical slices first to Y-A, then to Y-B, then to the fixed
Y-N controls. Y-A must produce a real terminal Response before shared-role
Merge work begins. Y-B must produce a real terminal result before the Y-N
matrix begins. The current fail-closed stub is removed only after all three
case paths share the same driver and cleanup owner.

1. **Harness adapter** — expose one case-runtime function around the maintained
   MiniNDN/NFD/SVS startup, routing, keychain, and process-supervision helpers.
   It consumes the validated `caseRuntime.nodes`, explicit runtime identity
   map, isolated policy, output root, and provider identity list; it must not
   trust the legacy runner's hard-coded `AI_LAB_*` names or preplanned role
   map. The binding is revalidated immediately before startup, including
   topology membership and policy digest.
   The APP policy loader's top-level `controller`/`group` and runtime
   `user_identity`/`provider_prefix` fields are explicit aliases and must
   match the runtime identity map; they are never inferred at process start.
   The adapter now exposes a side-effect-free `CaseProcessSpec` vector and a
   staged `start_processes(phase=...)` operations. Commands are generated from
   the isolated policy and explicit paths, with one capability Provider per
   declared identity and the maintained ACK-driven User flags. The live driver
   must start `control`, wait for Controller/repository markers, start
   `providers`, wait for every Provider marker, and only then publish the
   catalogue. The driver records the verified publication receipt with
   `mark_catalogue_published(...)` before starting `user`; unknown, repeated,
   out-of-order, or unacknowledged-publication phases fail closed. Unit tests
   inspect the vector and phase barriers before any MiniNDN process is created.
2. **Control and publication** — start Controller and repository, wait for their
   readiness markers, and have the runner orchestration publish the exact
   signed catalogue APP Data through `ServiceUser.publish_signed_app_data`.
   The maintained User process launched by the runner publishes the encrypted
   `REPO_REF` through `APPClient.publish_application_input_reference()` before
   `REQUEST_SENT`. Bind the actual coordinator `requestId` plus ACK attempt
   identity before the first lifecycle event. Controller's artifact-deployment
   manifest and an offline snapshot file are not substitutes for these
   publications.
3. **ACK-driven execution** — start only the authorized capability processes,
   run the maintained model-first User, and project the authenticated closed
   ACK snapshot into the sealed plan before Selection. The Provider-to-role map
   is evidence, never startup input.
4. **Oracle and cleanup** — validate role assembly, dependency fetch, numerical
   equivalence, redaction, child exit, and cleanup; emit the case marker only
   after the complete lifecycle and result schema pass. Y-N reuses these slices
   from fresh subcase directories and does not create a second harness.

   T010/T013 implementation repair (2026-09-04): the maintained User must load
   the registered fixed PPM fixture and its hash-bound, precomputed NumPy oracle,
   not `make_input()` random data or a deployed Torch forward pass. Use the
   registered float32 RGB/NCHW bilinear preprocessing and native tensor format.
   Validate finite canonical detection rows at atol=1e-3, rtol=1e-4 before any
   successful User marker; persist only identity/digest/aggregate-error evidence,
   never plaintext input or detection rows. This component record does not
   replace the candidate-bound terminal collector or actual CUDA EP evidence.

   T007/T013 native-evidence continuation: reuse `ExecutionEvidence` and its
   existing post-run observer. Record the actual process/visibility and the
   worker's request/attempt/cache disposition. Resolve CUDA physical UUID from
   the selected runtime device's PCI address through the CUDA driver, never
   from a caller-provided UUID. Enable uniquely named profiling for V3 assembled
   ONNX runners and finalize after the first non-warmup request; forbid CPU EP
   fallback when CUDA is required. Profile evidence must identify its request
   and reject CPU model-node placement. These implementation records still
   require the pending candidate-bound terminal collector and live validation.

Each slice requires a focused regression, but no slice may be marked as a
formal qualification result until T014 re-audits the complete production path.

## Migration and Rollback

- Keep `distributed_inference()` only as deprecated preplanned compatibility;
  remove it from the YOLO qualification entrypoint and examples used as current
  guidance.
- Add the generic model/task request path without breaking current Qwen callers;
  Qwen convenience arguments adapt into the same coordinator call.
- Keep the old pre-split YOLO outputs only as an offline numerical oracle until
  the new distributed equivalence tests pass; they cannot select Providers or
  count as runtime evidence.
- A feature flag may disable the new YOLO adapter registration and return to
  atomic/full-model execution, but may not bypass security or accept an
  uncertified cut.
- No remote rollback artifact is needed before Tiger because all candidate
  construction is local. A failed Tiger candidate is retained immutably and
  reported `UNQUALIFIED`.
