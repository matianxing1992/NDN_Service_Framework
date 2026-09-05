# Feature Specification: ACK-Driven YOLO Tiger Functional Slice

**Feature Branch**: `Experimental`

**Feature Directory**: `180-ack-driven-cross-model-qualification`

**Created**: 2026-09-02

**Status**: `IMPLEMENTATION_REPAIR_BEFORE_T014` (documentation revision 124,
2026-09-05);
T001, T003, T012, and T019 complete, T002 and T004--T011 partial, T013
partial, and T014--T018/T020 not complete. Live Y-N-I focused run r42 PASS
is recorded (2026-09-05). The strict negative-verdict repair (2026-09-05)
makes earlier Y-N PASS labels historical observations. The controlling
completion item is the FR-008 protected-grant subsystem (T014 BLOCK, owned
by T007/T010). The revision-121 exact-SIF/S4 evidence is invalidated by the
revision-123 runtime repair; S4 must be rerun from a new candidate.

**Current-source addendum (2026-09-04)**: the Python Provider compatibility
boundary now rejects invalid V3 Selection instead of falling back to legacy
execution, and preparation progress preserves the selected attempt. Native-only
Spec180 process ownership is unchanged. Focused repair evidence is recorded in
`evidence/provider-boundary-repair-20260904.md`; it is not a new T014 PASS or
qualification result. Source binding follows FR-015 and
`contracts/immutable-candidate-v1.md`, including explicitly recorded modified
and untracked bytes; a Git commit is not the contract's only binding mechanism.

**Latest native-evidence continuation (2026-09-04)**:
`evidence/t013-native-evidence-repair-20260904.md` records actual-process and
request observation wiring, queried CUDA identity, and first-request profiling
(99 focused checks). CUDA API calls were exercised with explicit test doubles,
not real hardware. The candidate-bound terminal collector, full cleanup, live
Y-N and candidate binding remain open. T014 is BLOCK; no SIF/Tiger rerun.

**Prior numerical checkpoint (2026-09-04)**:
`evidence/t013-numerical-repair-20260904.md` records the registered fixture
input and real-response numerical component producer (83 focused checks).
The component is wired into User's success decision; no model/network
qualification was run. Actual CUDA evidence, the candidate-bound terminal
collector, full cleanup, live Y-N and complete candidate binding remain open.
T014 remains BLOCK. Earlier source-only snapshots do not bind these new bytes.

**Prior supervision checkpoint (2026-09-04)**:
`evidence/t013-supervision-repair-20260904.md` records the native Tiger
startup/supervision and sealed launcher-path repair (44 focused checks).
Actual numerical/CUDA evidence production, full cleanup, live Y-N and complete
candidate binding remain open. The 408-file source-only checkpoint is not a
promoted candidate; T014 remains BLOCK and no SIF/Tiger rerun is authorized.

## Revision 123: Controller PUBPARAMS startup readiness repair

Revision 122's node-local SIF reproduction reached the Controller but the
publication `ServiceUser` could not fetch NAC-ABE public parameters. The
failure was a startup race: `AttributeAuthority` installs its `PUBPARAMS`
filter from the asynchronous success callback of prefix registration, while
the publication User was constructed immediately after the Controller
background thread was launched. This is a runtime readiness defect, not an
ACK disposition or placement-design change.

The current Core `ServiceController::start()` probes the real controller
`PUBPARAMS` Data endpoint on its Face/event loop with a bounded deadline before
returning. The Python Native Controller exposes that result through
`wait_until_ready()`, and the Python `start()`/`start_background()` wrappers
fail closed on timeout or startup error. Callback state remains alive after a
timeout, so a late Interest callback cannot reference a dead stack variable.
The focused test/build/import evidence is
`evidence/t013-controller-pubparams-readiness-current-20260904.md`.

Because this changes the Core/Python runtime plane, the revision-121 exact-SIF
hash is stale under the immutable-candidate rules. Ownership returns to T013,
then a fresh T014 `PASS`, current-source T015 local qualification, a new SIF,
and only afterward the single Tiger route. No SIF or Tiger experiment is
claimed for this repair.

## Revision 124: grant closure as the controlling completion item

The 2026-09-05 implementation repair makes the remaining completion order
explicit: every known open problem is now owned by an existing task, and
executing T001--T020 in completion order closes them all.

- The FR-008 protected-grant subsystem (authenticated policy authority,
  Provider-recipient-encrypted signed grant Data, plan finalization,
  Provider verify/unwrap, and a non-`plaintext-v1` production role default)
  is the controlling T014 blocker. It is implemented by T007 (positive
  path) and T010 (real grant mutation in Y-N-E); HMAC scaffolding and
  `plaintext-v1` defaults MUST NOT qualify. See
  `evidence/t008-protected-grant-gap-20260905.md`.
- Y-N verdicts are semantic: the runner MUST verify the registered
  rejection reason at the listed boundary; an unrelated failure does not
  satisfy any Y-N subcase. The 2026-09-05 repair (183 focused checks) and
  the live Y-N-I r42 PASS are the current Y-N baseline.
- The revision-121 S4 exact-SIF replay and its SIF hash are invalidated by
  the revision-123 Core/Python readiness repair. S4 is rerun only after a
  fresh T014 PASS, T015 local qualification, and a new candidate seal.
- The Controller readiness probe now verifies PUBPARAMS through an
  independent second Face (own transport/PIT/event loop) with a fresh
  random name suffix, so in-process ndn-cxx satisfaction cannot prove
  readiness (commit `8b24911b`). This supersedes the revision-123
  same-Face description.

## Revision 112: finite completion target (authoritative)

Spec180 now has one completion target: **one immutable YOLO candidate completes
one ACK-driven shared-backbone request on one Tiger compute node and returns a
numerically correct terminal Response through four independent Provider
processes**.

The first Tiger profile is intentionally small:

- one node and one GPU (one RTX GPU allocation);
- four independent Provider processes owning `BackboneNeck`, `DetectShard0`,
  `DetectShard1`, and `Merge` one-to-one;
- the three model-compute Providers use CUDA ONNX Runtime on the same physical
  GPU; `Merge` is an explicit CPU dependency-consumer role;
- one cold request, one full-model numerical oracle, complete lifecycle and
  child-exit evidence, and bounded cleanup;
- no throughput, latency, scaling, multi-GPU, or performance claim.

This profile proves that the real SIF, Tiger runtime, NDN control/data paths,
ACK-derived placement, four Provider processes, role assembly, CUDA execution,
dependency delivery, and final result work together. It does **not** prove that
work is accelerated across physical GPUs. A later qualification feature may
reuse the accepted candidate to add distinct-GPU placement, warm reuse, Qwen
3.6-27B, continuation, and performance experiments.

The active gate order is:

```text
S0  one -j2 ABI-identical native closure
S1  one role-correct signed YOLO experiment candidate
S2  local MiniNDN Y-A, then Y-B, then the minimum security/feasibility controls
S3  one code-aware convergence PASS and one YOLO-relevant local qualification
S4  one locally built SIF plus exact-SIF Y-B replay
S5  read-only Tiger readiness, accepted-byte staging, and one Tiger Y-B request
```

Only the first open gate is active. Qwen execution, three distinct GPUs, a
second warm request, and cross-model closure are no longer Spec180 completion
requirements. Earlier revision notes remain as diagnosis history; where they
conflict with this section, revision 112 controls.

For S1, release tooling may generate one experiment-only catalogue/Provider
trust set before candidate sealing. Private keys stay outside Git and the SIF;
the public identities and digests are frozen into the candidate. Runtime key
generation remains forbidden, and this functional result makes no production
PKI claim.

**Revision 108 completion diagnosis (2026-09-03)**: Spec180 has not yet
completed a real NDNSF-DI experiment for four separate reasons, and they must
not be conflated:

1. **The first runnable candidate is not closed.** Temporary packages are
   either using the obsolete `DetectHead0/1` role names or are missing the
   catalogue signature rooted in `trust-root-registry-v1.json`. The matching
   private signing key is owner-supplied; generating a replacement key or
   editing a manifest in place would invalidate the registered trust root.
2. **The live process inputs are incomplete.** One candidate manifest still
   does not bind the Provider offer-key files, their digests, and the
   canonical Y-A ONNX file as one immutable input set. The fail-closed runner
   therefore returns `WAITING_EXTERNAL_INPUT` before NFD/SVS, by design.
3. **Work was sequenced behind the first experiment.** Audit refinements,
   release checks, and negative/shared-role seams accumulated before one
   atomic `FullModel` Y-A request reached a terminal Response. Focused tests
   prove implementation or wiring; they cannot prove execution.
4. **Expensive validation is correctly downstream.** SIF, CUDA, and
   TigerCluster are blocked behind G0, one live Y-A, Y-B/Y-N, T013, and one
   fresh convergence PASS. Repeating those downstream actions cannot create
   the missing protocol evidence.

The only recovery path is therefore finite and owner-actionable: close the
native runtime gate, obtain one fresh role-correct signed package plus the
bound Provider/model inputs (G0),
run exactly one Y-A request through Controller/Repository/Provider/User to a
terminal Response (G1), reuse that driver for Y-B/Y-N (G2), then finish T013
and run T014 once. Until G0 closes, the correct result is
`WAITING_EXTERNAL_INPUT`, not a failed NDNSF-DI measurement and not another
preflight/SIF/Tiger loop.

**Revision 109 runtime-closure diagnosis (2026-09-03)**: The first developer
Y-A attempt progressed past input validation but failed during Controller and
Repository startup with `socket read error (End of file)`. Read-only linkage
evidence identified the cause before any request was sent: the Python/native
extension and NAC-ABE resolved `libndn-cxx.so.0.9.0` from the repository's
`.local-boost171/lib`, while MiniNDN's NFD and the current NDN-SVS library
resolved a different `/usr/local/lib` file with the same soname. Their
SHA-256 digests and ELF Build IDs differ. This is a split host ABI/toolchain,
not an NDNSF-DI protocol result. An ad-hoc `LD_LIBRARY_PATH` override is not a
repair: it either leaves NFD and the client split, or loads stale `/usr/local`
framework libraries and fails with missing current symbols. The runner now
checks the extension, NFD, NDN-SVS, and NAC-ABE closure before MiniNDN starts
and reports `WAITING_EXTERNAL_INPUT` (exit 78) on mismatch. The next build must
produce one ABI-identical closure for NFD, NDN-SVS, NDNSF, NAC-ABE, and the
Python extension, then rerun one Y-A request. Until that closure and terminal
Response exist, no Y-A/Y-B/Y-N protocol result is claimable and no SIF/Tiger
work may start.

**Revision 110 gate-order correction (2026-09-03)**: The native-library
failure is a named gate before candidate closure, not an incidental detail
inside a MiniNDN attempt. Spec180 therefore has two independent G0 inputs:

```text
G0-NATIVE   one ABI-identical NFD/NDN-SVS/NDNSF/NAC-ABE/Python closure
G0-CANDIDATE trusted role-correct YOLO package + signature + Provider/model inputs
G1-Y-A      one real Controller/Repository/Provider/User terminal Response
G2          reuse the same driver for Y-B and fixed Y-N controls
T013        release tooling, QWEN-F production input/entrypoint, and result writer
T014        one design-code convergence PASS
T015--T020  one frozen local/SIF/Tiger route
```

The delay is thus not a mysterious protocol failure. The first Y-A stopped at
`G0-NATIVE` with socket EOF before any request, and the candidate still lacks
owner-supplied trusted signing/model inputs. Focused tests, a publication seam,
or a temporary package prove only `implemented`/`wired`; they cannot advance
`executed`. The runner must emit one bounded `WAITING_EXTERNAL_INPUT` record
per open G0 gate, preserve the first failing layer, and resume at that gate
after the exact input is fixed. No SIF, Tiger, broad-suite, or matrix action
may be used to discover or mask a G0/G1 defect.

**Read this first (revision 106)**: Spec180 is not waiting for another broad
audit or another SIF build. It has one finite execution queue:

```text
G0-NATIVE ABI closure -> G0-CANDIDATE trusted YOLO input closure
  -> G1 one atomic Y-A request and terminal Response
  -> G2 reuse the same driver for Y-B and fixed Y-N controls
  -> finish T013 production manifest/object closure and QWEN-F execution evidence
  -> T014 one convergence PASS
  -> T015--T020 one frozen local/SIF/Tiger route
```

Only the first not-yet-passed gate is active. A later gate must not be started
early, and a focused rejection test cannot be promoted to an executed result.

**Revision 104 release boundary (historical; superseded by revision 106,
2026-09-03)**: The host release renderer
rejects a structurally invalid, unsigned, wrong-model, CPU-fallback-enabled,
or non-CUDA QWEN-F manifest before scheduler submission by using the
registered model-manifest trust root. At that point this was only a T013
implementation correction; revision 106 adds the separate entrypoint, but the
real Qwen3.6-27B manifest and execution evidence are still missing. T013
remains partial and the fresh T014 convergence audit is still required before
SIF promotion.

**Revision 105 scope/order correction (2026-09-03)**: The original task graph
was too coarse for a first vertical result. T002/T004--T010 contain both the
minimum atomic path and the later shared-role/negative matrix, so many tasks
could remain “partial” while no request was runnable. The operative split is
now explicit: finish only the atomic `FullModel` portions needed by G1; defer
`BackboneNeck`/`DetectShard0`/`DetectShard1`/`Merge` and the negative matrix
until G1 has a live terminal Response. The QWEN-F entrypoint and release
checks must be complete before T014/SIF sealing, but they are not a substitute
for G1. Missing G0 material is an external-input wait, not permission to
invent a package, edit a manifest in place, or repeat preflight work.

**Revision 106 T013 entrypoint correction (2026-09-03)**: The separate
`Experiments/NDNSF_DI_QwenAckDriven_Minindn.py --case QWEN-F` entrypoint now
exists. It performs the registered manifest check, incremental graph,
initializer, and stage digest checks, tokenizer/prompt binding, and fixed
CUDA three-stage request-first argument construction before delegating to the
maintained pipeline runner. Focused tests cover missing, mutated, and valid
fixture inputs. This advances T013 from “entrypoint absent” to
`implemented`; it does not provide a signed production Qwen3.6-27B manifest,
live ACK/Selection/Response evidence, or qualification.

**Revision 107 input-status correction (2026-09-03)**: The maintained YOLO
entrypoint now separates input validation from protocol execution. Missing or
invalid G0 inputs are reported as
`SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT` with exit code 78 before
NFD/SVS startup;
failures after `run_minindn_case()` begins remain `UNQUALIFIED`. This prevents
an absent candidate from being misreported as a negative NDNSF-DI result. It
does not close G0, create a terminal Response, or advance qualification.

**Implementation/evidence boundary (revision 108)**: The coordinator now
exposes bounded transition callbacks, and the maintained YOLO User path
instantiates `LifecycleJournal`, binds the explicit coordinator request and ACK
attempt, and emits the ten milestones on the production path. This is wired
source evidence only: no live Y-A process has yet produced the trace and
terminal Response, so T011 remains partial.

**Implementation/evidence boundary (revision 109)**: The maintained runner
now performs a native-library closure check before creating MiniNDN. It compares
the resolved and hashed `libndn-cxx` used by the Python extension, NDN-SVS,
NAC-ABE, and NFD. A mismatch is `WAITING_EXTERNAL_INPUT` (exit 78), not a
protocol failure. This closes a repeatable host-toolchain failure that had
previously appeared as a Controller/Repository socket EOF; it does not advance
T011. A real Y-A terminal Response is still required after rebuilding the
complete stack with one library closure.

**Current gate (2026-09-03, revision 110)**: `BLOCK` at G0-NATIVE before
T011-A input and execution readiness. The runner now has a barriered
Controller/Repository/Provider/User
startup path and candidate-bound catalogue publication code; focused tests and
Python compilation pass. No valid current Y-A run has started the live
exchange, however, because the temporary candidates are not admissible as a
sealed input: most use obsolete `DetectHead0/1` names, while the one observed
`DetectShard0/1` package has no trusted catalogue signature. The process vector
now validates and passes the Provider offer-signing key map and the canonical
local model path, but no admissible candidate manifest currently supplies
valid files for that binding. This is an incomplete candidate-input state,
not a failed model result. Until the native closure is rebuilt, a fresh
package is sealed, and one Y-A request reaches ACK closure, Selection,
Provider execution, and terminal Response, there is no protocol result to
measure.

The temporary `/tmp/spec180-yolo-*` packages inspected during T011 preparation
are inadmissible candidates. Most shared-candidate manifests use the obsolete
role names `DetectHead0` and `DetectHead1`; one package has the current
`DetectShard0` and `DetectShard1` names but its catalogue has no trusted
signature. The current exporter, adapter, policy, and tests require the
current names plus a valid registered signature. Packages must be regenerated
and revalidated by the current exporter. Renaming manifest fields in place
would sever the candidate/graph digest binding and is not a valid repair.

## Revision 103 baseline diagnosis (superseded by revision 105)

The delay is an input-and-ordering stop, not a slow NDNSF-DI execution.
Spec175 is already a sealed local baseline. Spec180 has source-level and
launch-level wiring, but it has not been allowed to claim a protocol result
because the first candidate is not admissible and the QWEN-F production
manifest/object set is not sealed into the same release candidate.

The only current queue is:

```text
G0: one fresh signed YOLO package + policy + offer keys + canonical ONNX
G1: one real Y-A Controller/Repo/Provider/User terminal Response
G2: reuse the driver for Y-B and fixed Y-N controls
T013: finish release tooling, production manifest/object closure, and the
separate QWEN-F ONNX execution evidence
T014: one design-code convergence PASS
T015--T020: one frozen local/SIF/Tiger qualification route
```

If G0 cannot be satisfied, the orchestration record is
`WAITING_EXTERNAL_INPUT` and the runner stops before NFD/SVS. The runner may
emit a more specific pre-start error such as `ENVIRONMENT_MISSING` or
`CANDIDATE_*`; the orchestration layer must map that error to the same
waiting state, not to an execution failure. The required owner-supplied inputs are the current
`DetectShard0/1` package, the registered catalogue signature, the Provider
offer-key map, and the canonical Y-A ONNX file with digests. An obsolete
`DetectHead0/1` package, an unsigned package, or a manifest edited in place is
not a fallback: it creates a different candidate identity and invalidates
downstream evidence.

The evidence vocabulary is binding: focused tests are `implemented` evidence,
a validated launch vector is `wired` evidence, a real terminal Response is
`executed` evidence, and only final oracle/cleanup agreement is `qualified`
evidence. Repeated preflight, SIF builds, or Tiger submissions cannot replace
the missing G1 result.

**Revision 104 delay record**: The current root cause and exact recovery queue
are recorded in
`evidence/revision104-delay-diagnosis-20260903.md`. This is a readiness block,
not a negative NDNSF-DI protocol result: the runner has not reached NFD/SVS for
the current candidate. The next executable actions are `G0-NATIVE` followed
by `G0-CANDIDATE`, then one Y-A request; another broad audit, SIF build, or
Tiger submission is not an acceptable substitute.

## Revision 102: completion diagnosis and bounded recovery

Spec180 has been slow because it was allowed to accumulate implementation
seams and audit notes before one runnable end-to-end request existed. The
current code has crossed the `implemented` and `wired` levels, but the first
`executed` level is still blocked. The blockers are concrete and independent:

1. **No admissible YOLO candidate**: temporary packages either advertise the
   obsolete `DetectHead0/1` roles or lack the trusted catalogue signature.
2. **Incomplete external input closure**: the live process vector now carries
   the Provider offer-key map and Y-A model path, but no one sealed manifest
   currently supplies valid key files, their digests, and the canonical ONNX
   file together with the package.
3. **Missing cross-model release input/evidence**: the QWEN-F entrypoint now
   exists and is focused-tested, but no signed Qwen3.6-27B ONNX manifest,
   in-image object set, or two-request terminal result is available to seal
   into the same candidate as YOLO.
4. **Evidence ordering**: focused tests and preflight checks prove rejection
   and wiring only; they cannot create a protocol result. T014, SIF, and
   Tiger are correctly blocked until a real Y-A terminal Response exists.

The recovery is deliberately finite: (G0) seal one fresh role-correct,
signed YOLO package plus policy, key, and model inputs; (G1) run exactly one
real Y-A Controller→Repository→Provider→User request and preserve its
terminal Response; (G2) reuse that driver for Y-B/Y-N, finish T013's QWEN-F
and release tooling, then run T014 once. No new benchmark matrix, SIF build,
or Tiger submission is permitted before G1/G2. A missing external artifact is
recorded as `WAITING_EXTERNAL_INPUT`, not worked around with an offline or
synthetic result.

## Revision 99 (historical diagnosis; superseded by revision 103)

Spec180 has not produced a real NDNSF-DI result because the first executable
candidate never crossed the candidate-readiness gate. The delay has four
concrete causes:

| Cause | Effect | Binding correction |
|---|---|---|
| Temporary packages mixed old `DetectHead*` roles with current `DetectShard*` roles | The adapter, catalogue, and Provider could not agree on one candidate identity | Re-export from the current exporter; reject in-place manifest edits |
| The role-correct package had no trusted catalogue signature | ACK-driven placement could not authenticate the runtime snapshot | Supply the registered signing key/certificate and verify its digest before startup |
| The runner lacked a complete case-policy/Provider input closure | A process could start with no authoritative nodes, identities, offer key, or local Y-A model | Require explicit runtime policy, absolute key map, and canonical ONNX path in T011-A |
| Audit/preflight refinements ran ahead of one live vertical slice | Many focused tests passed while no terminal Response existed | Stop broad work; execute only Y-A, then reuse that driver for Y-B/Y-N |

The corrected path is an input-closure gate followed by one real
Y-A Controller→Repository→Provider→User exchange. Only after a terminal
Response is observed may the same driver run Y-B and Y-N. Release/SIF tooling
and QWEN-F are completed before T014, and T015--T020 consume that frozen
candidate exactly once. Missing credentials or model artifacts remain an
explicit external-input block; they cannot be simulated with an offline
catalogue, injected publisher, historical Spec175 evidence, or a new SIF.

## Why the real experiment has not completed

The delay is not evidence that NDNSF-DI failed. The runner originally stopped
before network side effects; that old stub has now been replaced by a real,
barriered startup/publication path, but it is intentionally fail-closed for
invalid candidates. The current tree can therefore accumulate passing
contract and seam tests without producing a request result until all candidate
inputs are valid. The remaining production slice is precise: regenerate and
seal the current YOLO package, bind the Provider's Ed25519 offer key and
canonical ONNX path, then run Controller/Repository/Provider/User in order so
the signed catalogue, encrypted input reference, ACK-to-Selection flow,
Provider execution, lifecycle journal, terminal Response, and cleanup are all
observed in one case.

Three planning choices amplified the delay, and two input/wiring problems would
have caused the next failure even after the driver was wired:

1. T011 originally combined harness construction, publication, positive cases,
   negative cases, and formal evidence. That made boundary-test progress look
   like case progress while no single Y-A request was runnable.
2. Release/SIF work and the external QWEN-F executable were placed after
   convergence or sealing. That ordering could never produce one immutable
   candidate: later implementation changes would invalidate the image.
3. Historical audit iterations were recorded as a long narrative and were
   repeatedly mistaken for an execution queue. They are provenance only.
4. The temporary YOLO set is not a sealed release input: most packages use an
   older role vocabulary (`DetectHead*`), and the only observed package with
   `DetectShard*` has no trusted catalogue signature. The current graph-bound
   contract requires both the role vocabulary and registered signature;
   accepting either incomplete form would make the adapter/runtime disagree
   about the selected candidate identity.
5. Earlier runner revisions did not carry the Ed25519 offer-signing key or
   canonical local ONNX path. The current process vector now carries and
   validates both, but the required external key files and model path are not
   present in one sealed candidate manifest. Without the former, the V3 ACK
   offer cannot pass the checked-in trust-root verifier; without the latter,
   Y-A cannot execute the selected `FullModel` role deterministically.

The corrected evidence ladder is:

| Layer | Current state | What it proves |
|---|---|---|
| Implemented | validators, contracts, adapters, and runner seams exist | source-level behavior and fail-closed checks |
| Wired | the runner builds the validated process vector and can launch phases for a sealed candidate | startup/publication ownership is checked; no protocol result has yet been recorded |
| Executed | T011-A/B/C runs real MiniNDN/NFD/SVS and reaches a terminal Response | production request path is reachable |
| Qualified | T014--T020 evidence, child exits, security, numerical/runtime oracles, and cleanup agree | a candidate-specific local/SIF/Tiger verdict |

The current feature is between `wired` and `executed`, with T011-A blocked on
candidate input closure (including the external Provider key/model files).
Therefore no SIF build, Tiger job, or
historical Spec175 result can be used to fill the missing execution layer.
Until the first Y-A terminal Response, only focused tests and the smallest
developer MiniNDN smoke are authorized.

This is a deliberate candidate-readiness stop, not an experiment that has been
running slowly. The runner now has a production owner for the first live
request, but it correctly rejects the stale package and incomplete Provider
inputs before creating protocol evidence. Adding seeds, rebuilding SIFs, or
submitting Tiger jobs cannot produce useful Spec180 evidence yet; each would
either repeat the same input failure or create a candidate whose source,
runtime, and signing material are not sealed together.

**Why completion stalled**: work repeatedly stopped at boundary checks because
the plan allowed release/audit refinements before the one live driver existed.
Two ordering defects made that worse: SIF tooling was mixed into a later
execution task, and the QWEN-F executable was deferred until after SIF sealing.
Iteration 92 corrected the order, but it did not implement the missing driver.
The only active implementation queue is therefore T011-A → T011-B → T011-C →
T011-D; all broad local, SIF, and Tiger work remains blocked until that queue
and one T014 convergence audit pass.

**Iteration 95 input-integrity correction (2026-09-03)**: A read-only package
inspection found that the temporary packages under `/tmp/spec180-yolo-*` are
not a single valid release: most shared candidates advertise `DetectHead0/1`,
whereas the current exporter and runtime contract require `DetectShard0/1`;
the one role-correct package has no trusted catalogue signature. This is
recorded as an inadmissible candidate, not fixed by editing JSON in place.
T011-A must first produce a fresh signed package with the current exporter,
verify graph/initializer/catalogue digests and the adapter, and preserve its
manifest and source hashes before any live driver or SIF work.

**Iteration 92 execution recovery (2026-09-03)**: Spec180 has not reached a
real Tiger experiment because its registered YOLO entrypoint still terminates
with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`. T011 therefore cannot complete,
T014 cannot issue a convergence `PASS`, and T015--T020 cannot legally start.
The previous plan also placed implementation work after T014: T016 could still
change SIF tooling, while the missing QWEN-F executable was deferred to T019,
after the SIF would already have been sealed. This created circular gates and
encouraged repeated preflight/audit refinements instead of one runnable path.

The corrected execution order is now binding:

```text
Spec175 frozen local baseline
  -> finish one real Y-A ACK-to-Response developer slice
  -> extend the same driver to Y-B and the fixed Y-N negatives
  -> finish all release/SIF scripts and the QWEN-F executable
  -> one T014 design-code convergence audit
  -> one complete local qualification campaign
  -> one local SIF build and exact-SIF replay
  -> Tiger YOLO-F, then Tiger QWEN-F, then final closure
```

Before T014, run only focused tests and the smallest developer MiniNDN smoke
needed to close the current production seam. Do not rerun the complete local
suite, rebuild a SIF, upload, stage, or submit a Tiger job after every small
repair. After T014 passes, T015--T019 are execution-only gates: if any of them
requires a source, harness, recipe, or profile change, invalidate the candidate
and return to the owning pre-T014 implementation task and then T014.

The numbered iteration notes below are historical rationale unless explicitly
marked as the current iteration. They do not add new gates or change the
iteration-101 execution order.

**Iteration 96 source correction (historical; superseded by revision 103,
2026-09-03)**: The registered YOLO
runner no longer has the old unconditional `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`
stop. It now validates a candidate-bound publication batch, launches the
maintained phases in barrier order, and waits for a controller readback before
starting User. This changes the implementation state from `unwired` to
`wired-at-launch`; it does not create a protocol result. The current candidate
is still blocked because the temporary candidates are either role-incompatible
or unsigned, and the external Provider input must supply an absolute Ed25519
private-key map plus the Y-A canonical ONNX path. The current process vector
validates and passes these inputs; they must be regenerated/sealed in one
candidate manifest before a developer Y-A request is attempted. Focused checks are evidence of this gate
only; no Y-A/Y-B/Y-N, SIF, CUDA, or Tiger qualification is promoted.

**Iteration 97 controller-entry correction (2026-09-03)**: A source-level
inspection found a second launch blocker that the previous checkpoint had not
captured. `controller.py` entered its ordinary long-running `controller.run()`
path whenever `--deploy-to-repo-manifest` was absent, even when the runner had
provided `--spec180-runtime-publication-file`. Consequently the controller
never called `_publish_spec180_runtime()`, no signed catalogue/artifact receipt
could be created, and `run_minindn_case()` could only time out waiting for it.
The controller now treats the Spec180 publication file as an independent
controller-owned mode; a repository deployment manifest is required only for
the unrelated repository-deployment path. This closes a production wiring
defect, but does not close T011-A: the fresh role-correct package, Provider
offer keys, local model, and one live terminal Response are still required.

**Iteration 98 publication-envelope correction (2026-09-03)**: The
Controller-side publication input crosses a process boundary and is therefore
treated as untrusted. It now validates the case, signer-scoped catalogue name,
package-manifest digest, exact catalogue digest, artifact-name namespace,
duplicate names, bounded payload size, and every artifact payload digest before
starting the signing User. A matching catalogue digest alone is not sufficient
to accept a tampered artifact batch. This closes an input-integrity seam; it
does not create live protocol evidence or change the T011-A candidate blockers.

**Iteration 91 integrity correction (2026-09-03)**: The runtime catalogue
adapter now requires exactly one snapshot for every candidate registered by
the case, rejects unexpected candidate digests, and requires the snapshot's
role/artifact-name map to equal the case's complete role set with non-empty,
unique absolute NDN names. Exact APP readback also now requires a non-empty
certificate whose name is rooted at the configured controller signer; a
successful fetch with no signer certificate is rejected. These are
fail-closed adapter checks, not evidence that the production publisher or
ACK-driven driver is wired. The focused runner slice is 47 passing tests; the
four bounded groups now report 173 passing tests with 20 existing
exporter/runtime warnings.

**Iteration 88 implementation correction (2026-09-03)**: The MiniNDN runtime
adapter now owns a strict runtime catalogue publication seam: it composes the
candidate-bound payload, calls the controller `ServiceUser` publisher, reads
back the exact name through the expected-signer path, compares the full
payload bytes, and records the receipt only after that readback. Publish
failure, readback failure, name/signer mismatch, payload mutation, invalid
timeouts, and duplicate publication fail closed. The publisher remains
injected so focused tests do not create MiniNDN; the production driver still
has to supply the controller-node ServiceUser and invoke this seam before
starting User. The focused runner slice now has 44 passing tests and the full
Spec180 collection reports 170 passing tests with 22 warnings after this
correction. This closes a
publication seam only; it does not claim live ACK/Selection/Provider/Response
execution or any MiniNDN/SIF/Tiger qualification.

**Iteration 89 cleanup correction (2026-09-03)**: `MiniNdnCaseRuntime` now
owns the child-process handles it starts and exposes an idempotent `stop()`
teardown that stops only those children, stops the case network, invokes the
maintained MiniNDN cleanup, and clears phase/publication state. The live T011
driver must call this method from `finally` on every success or failure path;
the focused runner regression covers one-child cleanup and repeated teardown.
This closes the adapter cleanup seam only. The production ACK-driven driver,
candidate-bound artifact publication, and T014 convergence audit remain open,
so no MiniNDN/SIF/Tiger qualification is implied.

**Iteration 90 idempotence correction (2026-09-03)**: The cleanup seam now
records completion before invoking teardown and returns without touching global
MiniNDN state on repeated calls. A stopped runtime rejects a later network
restart, preventing a second cleanup call from affecting another active case.
The focused regression covers the one-time cleanup and restart rejection. The
production ACK-driven driver and T014 convergence audit remain open. The
current focused runner slice has 45 passing tests; four bounded Spec180 Python
groups report 171 passing tests with 20 existing exporter/runtime warnings.

**Iteration 87 design/code correction (2026-09-03)**: The signed runtime
catalogue resolver now rejects non-`ACTIVE` snapshots at the APP parse
boundary. `RETIRED` and `REVOKED` records remain valid historical catalog
objects, but they cannot participate in the runtime snapshot used for a new
request. A focused resolver regression covers a retired record; this prevents
stale or revoked metadata from reaching placement. The complete Spec180
Python collection remains 169 passing with 22 warnings. This is a fail-closed
status guard only; it does not advance T011/T014 or authorize MiniNDN, SIF, or
Tiger validation.

**Iteration 86 design/code correction (2026-09-03)**: The signed runtime
catalogue resolver now rejects duplicate `candidateDigest` entries in addition
to duplicate aliases and manifests. The runtime contract allows at most one
ACTIVE snapshot per registered candidate; rejecting this ambiguity at the
signed APP parse boundary prevents later placement from seeing a non-unique
candidate publication. A focused resolver regression covers a distinct
alias/manifest carrying the same candidate digest. The complete Spec180 Python
collection remains 169 passing with 22 warnings. This is a fail-closed parser
correction only; it does not advance T011/T014 or authorize MiniNDN, SIF, or
Tiger validation.

**Iteration 85 design/code correction (2026-09-03)**: The V3 maintained path
now invokes the existing artifact-preparation authority when an explicit
signed catalog resolver is supplied. Previously it could seal synthetic role
artifact names while dynamic provisioning was disabled, producing a Selection
that no Provider could fetch. A focused regression now proves that a matching
ACTIVE snapshot resolves every role/rank before Selection; the focused suite is
20 passing and the complete Spec180 Python collection now reports 169 passing with
22 warnings. This repairs the pre-Selection artifact boundary only: no live
NDN execution or qualification evidence exists.

The audit also records an unresolved HIGH boundary: Spec180 has two distinct
catalogue records. The offline/package candidate catalogue
(`spec180-yolo-catalogue-v1`) identifies certified candidates but does not
contain runtime role artifact Data names. The active runtime snapshot
(`ndnsf-di-presplit-catalog-snapshot-v1`) is the signed APP Data record that
the User resolves after ACK closure and must contain candidate-bound
role/rank `artifactDataNames`. T011 must publish both candidate-bound
pre-split artifacts and this active snapshot before starting the User; merely
republishing the package catalogue, a legacy `/Stage` manifest, or an offline
snapshot cannot satisfy the contract. Until that publisher and the live
ACK/Selection/Provider/Response driver exist, T011/T014 and all MiniNDN, SIF,
and Tiger gates remain open.

**Iteration 84 naming/adapter correction (2026-09-03)**: A source-aware audit
compared the runner contract with the native `ServiceUser` APP Data
implementation. The catalogue record must be named below
`/<controller>/NDNSF/DI/`, with the signer equal to the controller identity;
the previous free-standing `/spec180/catalogue/v1` example could never be
published by the maintained native path. Runner validation and all focused
fixtures now enforce this binding. The audit also fixed an undefined identity
lookup in `initialize_keychains()` and a missing `providerPrefix` guard that
previously raised `KeyError` instead of the contracted fail-closed
`RunnerError`. The focused runner slice is 43 passing tests and the complete
Spec180 Python collection is 169 passing with 22 existing exporter/runtime
warnings. Startup-phase launch is failure-atomic and rolls back only children
created by a failed phase. These are preflight/adapter checks only; the real ACK-driven driver
remains unwired, so T011/T014 and all MiniNDN, SIF, and Tiger qualification
gates remain open.

**Iteration 73 evidence refresh (historical; superseded by iteration 78; 2026-09-03)**: The current Spec180 Python
collection is 138 passing tests with 22 warnings. This is focused source
evidence only. The semantic YOLO partition repair is retained from iteration
71. T011 now has a tested thin in-image dispatcher that verifies the closed
candidate-digest-bound workload schema, digest, fixed gate/case/arguments,
allow-listed environment, mount paths, and fresh evidence root before `exec`.
The real ACK-driven MiniNDN driver remains fail-closed; no live NDN
ACK/Selection/Provider/Response run, MiniNDN qualification, SIF replay, or
Tiger result exists. The mandatory T014 design-code convergence audit remains
the next qualification gate.

The dispatcher is an execution-boundary implementation, not a protocol
implementation: it does not select Providers, assign roles, infer model cuts,
or accept ambient source/model paths. The QWEN-F mapping is intentionally a
reserved entrypoint for the external Qwen3.6-27B ONNX runner; it must not
route to the Spec175 M11 tiny-model wrapper. Under the iteration-92 ownership
correction, dispatch failed closed before any Qwen workload started until T013
wired that production entrypoint. Revision 106 now supplies the focused-tested
entrypoint; dispatch still fails closed when the signed production manifest or
object set is absent, and the external model gate remains unqualified.

**Iteration 74 evidence refresh (historical; superseded by iteration 78; 2026-09-03)**: The host release validator
now consumes the same dispatcher schema before scheduler submission, and
`run-functional.sh` rechecks the mounted model-manifest and workload digests
before creating the writable evidence root. The complete focused collection is
now 140 passing tests with 22 warnings. These are fail-closed boundary checks
only; the ACK-driven MiniNDN driver still has no live execution evidence.

**Iteration 75 evidence refresh (historical; superseded by iteration 78 and
revision 106; 2026-09-03)**: The QWEN-F dispatch vector is now reserved for a
separate Qwen3.6-27B ONNX entrypoint and no longer routes to the Spec175 M11
tiny-model wrapper. At that time the host release validator and in-image
dispatcher both failed closed until that entrypoint existed, and the workload
carried only non-path Qwen identity metadata. Revision 106 adds the focused-
tested entrypoint; a signed production object set and live result are still
required. These boundary checks do not constitute Qwen, MiniNDN, SIF, CUDA,
or Tiger qualification.

**Iteration 76 evidence refresh (historical; superseded by iteration 78; 2026-09-03)**: The audit found that the
local-suite inventory had drifted from the frozen Qwen reference manifest:
both Q-C and Q-W were invoking the wrapper with the historical seed
`1750001`, while the manifest binds `175021` and `175022`, respectively. The
inventory now reads each source-case/seed tuple from
`qwen-reference-manifest-v1.json`, and its regression test covers the binding.
The terminal result validator now also checks the fixed two-request counts,
three-device CUDA ONNX Runtime map, no CPU model fallback, unique child IDs,
and cleanup counts instead of accepting structurally valid but semantically
false oracle fields. The focused collection is 143 passing tests with 22
warnings. These repairs remain boundary evidence only: the real Y-A/Y-B/Y-N
driver, T014 convergence audit, MiniNDN, SIF, and Tiger qualification are
still not complete.

**Iteration 77 design audit (2026-09-03)**: A contract review found that the
`Y-N-C` negative was underspecified: removing only one shared-role capability
could still leave `atomic-v1` feasible, so the declared no-feasible-candidate
outcome was not deterministic. The negative is now defined as a closed ACK
snapshot that removes the `FullModel` capability and at least one required
shared-candidate capability, making both registered candidates infeasible.
The audit also makes T011 reuse the existing MiniNDN startup, routing,
certificate, and process-supervision helpers from
`Experiments/NDNSF_DI_Yolo2x2_Minindn.py`; the new runner may adapt their
inputs, but must not fork a second NFD/SVS/security harness or invoke the
legacy deployment-first `main()` as a qualification oracle. No task status or
qualification evidence changes; the real driver remains fail-closed.

**Iteration 78 profile correction (historical; superseded by iteration 79; 2026-09-03)**: The runner audit found that
role-coverage-only startup validation could accept a degenerate Y-B/Y-N policy
with too few usable Provider identities, making the multi-Provider case
unexercisable. The fixed profiles now require exactly one Provider for Y-A,
exactly four for Y-B, and exactly four for Y-N. Y-B must have a distinct
capability cover for its four roles; Y-N must have that cover plus a
`FullModel` capability. This remains a startup capability witness only: ACK
closure and the sealed plan still own request-time assignment. The lifecycle
journal now requires the live driver to bind the coordinator's `requestId` and
ACK attempt identity before emitting an event; isolated journal tests may use
provisional IDs only to exercise the schema. This prevents the required
request/attempt lineage from being inferred from timestamps. Focused runner
tests pass; the real NFD/SVS driver and formal qualification remain blocked.

**Iteration 79 lineage correction (2026-09-03)**: A code-aware audit found
that generating `requestId`/`attemptId` inside the evidence writer would permit
a future driver to emit a plausible trace that was not bound to the actual
coordinator and ACK exchange. The journal now has an explicit production
binding mode enabled by default: the live driver must provide both identities
before the first milestone, while provisional IDs require an explicit opt-out
and are limited to isolated journal tests.
Rebinding or emitting an unbound production event fails closed. This correction
does not advance T011 or T014; the real driver and formal qualification remain
blocked.

**Iteration 80 harness-boundary correction (2026-09-03)**: The runner now has
an explicit `CaseRuntimeBinding`/`MiniNdnCaseRuntime` seam around the maintained
MiniNDN harness. Case inputs must declare runtime controller/user/repository/
group/provider identities in addition to node names; provider identity keys
must match the node map exactly. The binding rechecks topology membership and
the isolated policy digest before a future call to `Minindn.start()`, and its
routing plan merges origins when several logical processes share one MiniNDN
node. This closes the hard-coded-node ambiguity found by the audit but does
not wire ACK/Selection/Response execution. The current driver remains
fail-closed and T011/T014 are still open.

**Iteration 81 policy-loader boundary (2026-09-03)**: Before any MiniNDN/NFD
startup, the runner now applies the maintained `policy.py` parser and
compatibility checks to both the candidate source policy and the isolated
case-local policy that child processes will consume. This prevents malformed
service descriptors or missing user authorization from being discovered only
after child launch. The current focused runner slice has 28 passing tests and
the complete Spec180 Python collection has 153 passing tests with 22 existing
exporter/runtime warnings. These are preflight/source checks only; the live
ACK-driven driver, T014 convergence, MiniNDN qualification, SIF replay, and
Tiger execution remain unqualified.

**Iteration 83 staged-startup correction (2026-09-03)**: Comparing the case
contract with the launch implementation found that an ordered vector alone was
insufficient: the prior `start_processes()` call could launch User before the
catalogue publication barrier. `CaseProcessSpec` now carries explicit
`control`, `providers`, or `user` phases. The live runner must start control,
wait for Controller/repository markers, start providers, wait for every
Provider marker, publish the signed catalogue, and only then start User.
Unknown/repeated phases, missing phase nodes, early child exit, and readiness
timeouts fail closed. This is focused harness evidence only; the real driver
still remains fail-closed and T011/T014 are open.

**Iteration 82 process-vector boundary (historical; superseded by iteration 83)**:
The case runtime now
derives a side-effect-free, candidate-bound command vector for the Controller,
repository, each authorized capability Provider, and the ACK-driven User. It
uses the isolated policy, explicit runtime identities, topology node map, and
candidate paths; it does not call the legacy deployment-first role mapper or
fall back to hard-coded AI-LAB names. `start_processes()` reuses the maintained
`python_cmd`/`start` supervision helpers, but `run_minindn_case()` still fails
closed until live Controller/catalogue/input publication, ACK/Selection/
Provider/Response exchange, lifecycle binding, and the result oracle are
connected. The focused runner slice was 31 passing tests and the complete
Spec180 Python collection was 156 passing tests with 22 warnings. This is
preflight/source evidence only; no MiniNDN, SIF, or Tiger qualification is
claimed.

**Iteration 69 audit correction (historical; superseded by iteration 71)**: A
current-source audit found
four implementation-boundary gaps that keep the status unchanged. The YOLO
exporter and adapter still derive shared-role cuts from graph-node percentages
instead of proving semantic branch/tensor interfaces; the remote job wrapper
probes `/bundle/scripts/run_spec180_case.py`, but that dispatch entrypoint has
not been created; the terminal-result validator accepts label-only `PASS`
fields without candidate-bound evidence references; and the lifecycle journal
rejects obvious secret names but still accepts an arbitrary field such as
`payload`. The requirements below now make these checks explicit. T004/T005,
T011/T013, and T014 remain open; no MiniNDN, SIF, or Tiger result is promoted.

**Iteration 70 implementation correction (2026-09-03)**: The lifecycle journal
now enforces the closed milestone-specific field allowlist in the runner and
rejects generic or nested data fields. Focused regression coverage passes;
the terminal-result validator now requires structured candidate-bound oracle
records and verifies their evidence-file digests under an explicit root.
These are evidence-integrity hardening changes only and do not represent a
live ACK/Selection/Provider/Response result.

**Iteration 71 implementation correction (2026-09-03)**: The YOLO exporter and
adapter no longer derive shared-role boundaries from node-count percentages or
fixed topological indexes. The canonical package now records architecture-scope
node sets, lifted-constant ownership, producer/consumer tensor interfaces,
role dependency edges, semantic safe cuts, and a full-model equivalence oracle;
the adapter verifies those records against the loaded ONNX graph before it
creates a runtime candidate. This closes the heuristic-cut audit finding at the
exporter/adapter boundary only. Native assembly, the live ACK-to-Response
driver, and qualification remain open. The oracle metadata explicitly records
that the reference is generated by PyTorch and compared against CPU ONNX
Runtime; it is not presented as a standalone ONNX-only measurement.

**Iteration 66 audit correction (historical; superseded by iteration 68)**: The candidate-bound MiniNDN entrypoint now
materializes an isolated `case-policy.json` in the fresh case directory. It
filters the supplied authorization policy to the registered Y-A/Y-B/Y-N role
set without changing the source policy or assigning a Provider; the digest is
bound in `case-input.json`. This prevents unrelated roles/Providers from
silently entering a case. The focused runner tests now pass 16 tests (37 in
the combined runner/local-gate/inventory/contract slice). The real NFD/
NDN-SVS driver, candidate-bound inventory execution, and live ACK-to-Response
trace remain open.

**Iteration 68 design correction**: The previous startup-policy wording
incorrectly called `service.providers[*].roles` a fixed Provider owner and
rejected a Provider advertising more than one role. Those entries are only an
authorization/capability allowlist used to start the MiniNDN processes. The
runner now requires every registered role to be advertised by at least one
authorized Provider, permits overlapping capability advertisements, and keeps
the node map separate from placement. The selected Provider for each complete
role is still chosen only from the authenticated post-ACK snapshot and sealed
plan; no startup policy or case descriptor may preassign it.

**Iteration 67 audit correction (historical; superseded by iteration 68)**: The task, plan, audit, and traceability
records were compared against the current source and rerun gates. T012 is
confirmed complete, the combined runner/local-gate/inventory/contract slice
is 37 passing tests, and the full current Spec180 Python collection is 122
passing tests with 19 existing exporter/runtime warnings. The structural and
document/trust-root gates pass, but `run_minindn_case` still fails closed with
`ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no live MiniNDN, SIF, or Tiger result is
claimed. Formal validation therefore remains blocked at T011/T014, and the
next implementation task is still the real NFD/NDN-SVS ACK-to-Response driver.

**Iteration 65 audit correction (historical; superseded by iteration 68)**: The
candidate-bound MiniNDN entrypoint validated the case policy before any
NFD/Provider startup, but described the role allowlist as one fixed Provider
owner per role. Iteration 68 corrects that terminology and validation so
startup authorization cannot be mistaken for request-time placement.

**Iteration 64 audit correction**: A current-source re-audit found no new
qualification evidence. The candidate-bound Provider-offer verifier,
source-bound YOLO preflight, case-plan descriptor, lifecycle journal, inventory
generator, and local-gate supervisor remain focused/preflight implementations;
the real NFD/NDN-SVS Y-A/Y-B/Y-N driver, candidate-bound inventory execution,
and live ACK-to-Response trace remain open. The current status therefore keeps
T010, T011, and T013 explicitly partial rather than treating their focused
checks as completion. The terminology audit also distinguishes the maintained
candidate-bound Ed25519 policy/PEM map from the historical caller HMAC fixture;
neither is a substitute for native Trust-Schema certificate-chain execution.

**Iteration 52 correction**: The native ACK projection now carries an explicit
`trustSchemaValidated` provenance bit, and the maintained YOLO caller uses the
candidate-bound Ed25519 Provider-offer policy/key map rather than the
fixture-only HMAC map. This remains focused implementation evidence: the real
certificate-chain execution and Y-A/Y-B/Y-N runner are still required.

**Iteration 53 correction**: The production ACK SVS subscription now requests
packet-backed callbacks (`packets=true`). Without this, the validated Data
packet was discarded before `ServiceUser` could project its signer, KeyLocator,
wire digest, and Trust-Schema provenance into the V3 planning boundary. A
focused wiring regression and rebuilt native library cover this requirement;
live certificate-chain execution and the Y-A/Y-B/Y-N runner remain open.

**Iteration 54 boundary correction**: Spec175 is accepted only as a frozen
`LOCAL_FUNCTIONAL_PASS` baseline at its named source seal. The current
worktree contains a separate Spec180 source delta, including the ACK-provenance
and YOLO planning changes; a current-tree G0 seal verifies dirty bytes but does
not inherit Spec175's local-suite or MiniNDN results. Spec180 must therefore
record the delta, rerun its design-code convergence, and execute the live
ACK-to-Response local cases before any task is promoted from focused evidence
to qualification evidence.

**Iteration 55 execution correction**: The Y-N case is now a fixed negative
matrix with one positive catalogue-order control. Reversing catalogue order
must preserve the same decision; it is not a failure case. Each negative
subcase runs from a fresh MiniNDN state/output subdirectory and records its
expected boundary outcome before the aggregate Y-N marker is emitted. The
runner also records one non-secret JSONL event for each required lifecycle
milestone, rejects duplicate or out-of-order milestones, and emits the case
marker only after all Y-N subcases have reached their declared outcomes. This
removes the previous ambiguity without adding another inventory entry or
another placement implementation.

**Iteration 60 runner-boundary correction**: The registered Y-A/Y-B/Y-N
entrypoint now treats the canonical package directory as an explicit absolute
input, validates the catalog Data name and signer identity, requires the
candidate-bound Provider-offer trust-root schema, and persists the trust-root,
public-key-map, private-key-map content digest, and per-key digests in
`case-input.json`; private key paths and bytes are never recorded. Secret-bearing
metadata is rejected recursively rather than only at the manifest's top level.
Graph and initializer paths are confined to the canonical package root. These
checks remain preflight evidence; the real NFD/NDN-SVS driver and live
ACK-to-Response qualification are still open.

**Iteration 62 case-plan correction**: The runner now materializes a
candidate-bound `case-plan.json` after package validation. It records only the
signed candidate IDs, role sets, ingress/egress identities, priorities, and the
fixed Y-N outcomes; it contains no Provider names or role assignments. The
file is an execution input descriptor, not a placement decision: Provider
assignment remains owned by the authenticated post-ACK snapshot.

**Execution contract**: The maintained path is one request-scoped flow:
publish and verify the encrypted input reference, publish one generic request,
collect authenticated Provider ACKs until the registered 1500 ms window
closes, inspect the closed snapshot, choose a certified candidate, commit one
Selection, fetch only the selected role's inputs, and accept one terminal
result. YOLO is one-shot inference with a terminal result; Qwen is the separate
prefill-plus-automatic-decode stream. The two cases share transport and
security owners but must not be conflated by one generic oracle.

**Input**: Complete the real YOLO ACK-driven path and qualify YOLO and Qwen on
TigerCluster without expanding Spec175 or turning qualification into a
performance campaign.

**Boundary**: Spec175 has now handed off a named `LOCAL_FUNCTIONAL_PASS`
baseline. Spec180 may import Q-C/Q-W only through that sealed handoff and must
record its own source delta before treating them as current-candidate evidence;
the inherited pass is not substituted for Spec180 validation. The only Tiger
model subjects are the canonical YOLO26n ONNX package and a separately
identified, signed Qwen3.6-27B ONNX package; a small Qwen packaging smoke is
never a Qwen qualification result.

**Design review correction (iteration 21)**: The generic YOLO request is
`InferenceClient.request_task(model, task, input, timeout_ms, options,
strategy)`, or the equivalent generic `InferenceApplication.request(...,
task=..., task_options=..., timeout_ms=...)`. `InferenceClient.request_model()`
remains the `GenerationInput`/`GenerationConfig` convenience path and is not a
generic YOLO API. After Selection, the sealed V3 dataflow must contain an
`APPLICATION_INPUT` endpoint consumed only by the candidate-declared ingress
role and exactly one terminal-result owner equal to the candidate's egress role;
these identities are validated in the Provider context rather than inferred
from catalogue order or role names.

**Design/code audit correction (iteration 28, historical; superseded by
iterations 30 and 49--52)**: The generic facade was implemented, and the then-
maintained YOLO example still used a caller-provided offer-key map while its
CLI defaulted to and enforced the registered 1500 ms ACK window. The live
capability authority was intended to be the network `CollaborationAckClosed`
snapshot produced by the existing collaboration path. A
`PreSplitCatalogSnapshot` is only artifact-publication metadata and must be
obtained through an authenticated repository-backed resolver; it is not an ACK
snapshot and cannot decide placement. The previous file-backed snapshot is
offline-oracle-only; the maintained path now uses the signed APP Data resolver
introduced in iteration 30. Candidate ingress/egress
identities and the V3 `APPLICATION_INPUT` projection are implemented and
focused-tested. The Provider `REPO_REF` boundary now requires the
reference-aware fetch primitive and verifies encryption, authorization scope,
size, and content digest before returning plaintext; name-only or size-only
fetches fail closed. T002/T006/T009 remain open until the real ACK/Selection/
Provider/Response path is exercised, while T005 remains open until the
registered signed package is consumed without a test-only signature bypass.

**Design/code audit correction (iteration 29)**: The V2 compatibility wrapper
now initializes ownership-enforcement flags to disabled and derives them only
from a validated V3 `RoleDataflowContract`. Ordinary V2 handlers therefore do
not depend on V3-only state or fail before execution; the Provider-generation
compatibility regression covers this boundary. This repair does not close the
separate live ACK authority, signed-artifact, native/wire, or production
qualification gates.

**Design/code audit correction (iteration 30, historical; superseded by
iterations 49--52)**: The maintained YOLO path then
resolves active pre-split metadata through an exact-name, signer-checked APP
Data record using `NetworkCatalogSnapshotResolver`. The record is parsed as a
canonical, digest-bound snapshot envelope only after the coordinator receives
`CollaborationAckClosed`; it cannot close ACK collection or choose a candidate.
The old catalogue snapshot file is retained only for the explicit offline
oracle. Provider-offer verification still uses the existing key-map scaffold
and was not qualification authority at that point. The later candidate-bound
Ed25519 verifier/provenance work narrowed that gap, while production
certificate-chain verification and the ACK-to-Response trace remain open under
T006/T009/T011.

**Design/code audit correction (iteration 32, historical)**: The audit now separates
focused implementation evidence from production execution evidence. The current
Python regressions cover the request envelope, signed catalogue resolver,
candidate-local planning, role ownership, and input-integrity guards only. No
Spec180 MiniNDN case, native ACK-to-Response run, local qualification gate,
SIF replay, or Tiger job has passed. T010--T011 and T013--T020 therefore remain
open and must not be reported as implementation-complete merely because their
contracts or file paths are present. At the current iteration, T010 and T013
are partial; T011 and T014--T020 remain unstarted. The generic synthetic
object-detection adapter and the caller HMAC offer-key map remain
compatibility/fixture code; neither may feed a local, SIF, or Tiger
qualification result.

**Design/code audit correction (iteration 34)**: The iteration-33 publication
gap is now closed at the maintained source boundary. The canonical
`app_sdk.client.APPClient` exposes
`publish_application_input_reference()`, which calls the native encrypted
publisher, verifies the returned Data name, object ID, plaintext size, content
digest, authorization scope, protection epoch, encryption bit, and canonical
publication-manifest digest, and records only non-secret
`INPUT_REFERENCE_PUBLISHED` metadata. The native C++ result and Python binding
now expose those security-bearing fields; an old binding that returns only a
name/object ID fails closed. The maintained YOLO helper publishes its encoded
input through this method and rejects `--input-reference-file` in ACK-driven
mode. `APPClient.request_task()` requires the resulting reference digest to
match the durable publication journal before it can emit `REQUEST_SENT`.
Focused source and binding regressions pass, but T002/T009/T011 remain partial:
the real NDN ACK-to-Selection-to-Provider-to-Response path has not run, so no
MiniNDN, SIF, CUDA, or Tiger qualification is implied.

**Historical verification refresh (iteration 35)**: The complete Spec180 focused
Python collection passes 71 tests with 19 warnings; the publication/security
subset passes 25 tests. This refresh changes evidence counts only. It does
not close T002/T009/T011, does not provide a live ACK-to-Response trace, and
does not authorize MiniNDN, SIF, CUDA, or Tiger qualification before T014.

**Contract clarification (iteration 36)**: In the current NDNSF large-data
publisher/fetch path, `contentDigest` is the SHA-256 digest of the plaintext
returned after authenticated decryption. The legacy wire field
`ciphertextDigest` is retained as a compatibility alias for that same
post-decryption digest; it is not a digest of encrypted segment wire bytes.
The publication-manifest digest binds this meaning together with the Data
name, object ID, size, authorization scope, protection epoch, and encryption
flag. Any future wire-level ciphertext digest would be a separate field and
contract, not an implicit reinterpretation of the current alias.

**Local-inventory correction (iteration 41)**: The registered Q-C/Q-W entries
invoke the maintained `NDNSF_DI_StreamedGeneration_Minindn.py` wrapper rather
than calling the lower pipeline script with an incomplete `--spec175-case`
fragment. The wrapper supplies the fixed M01/M11, seed, tiny-ONNX, V3,
four-stage, and 5-second settle arguments and validates its terminal evidence.
The local supervisor supplies a unique output directory per child, requires
the explicit `SPEC175_RUN_REAL_MININDN=1` switch, and rejects stale evidence or
source/command substitutions before any child starts.

**Audit correction (iteration 42, historical)**: The local qualification inventory is the
single execution plan. Each registered formal case (Y-A, Y-B, Y-N, Q-C, and
Q-W) runs exactly once as its own supervised inventory child; a second
formal-case pass is not part of T015 and cannot create additional qualification
evidence. The then-current focused collection was 91 tests with 19 warnings; the
release, inventory, and local-gate slices are 6, 5, and 9 tests respectively.

**Audit correction (iteration 43)**: The checked-in YOLO example policy still
contains the historical `/Stage/...` deployment-first roles and is not a valid
Y-A/Y-B/Y-N policy. The real runner must create an isolated candidate policy
from the signed catalogue/selected plan: Y-A advertises only `FullModel`, while
Y-B registers four distinct component-role capability advertisements for
`BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`. The legacy policy
remains offline-oracle-only.
The runner inputs, fail-before-start rules, protocol oracle, and trust boundary
are defined in `contracts/yolo-minindn-runner-v1.md`; the caller HMAC key map
remains fixture-only and cannot qualify a local, SIF, or Tiger result.

**Audit correction (iteration 44, historical; superseded by iteration 45)**:
The canonical YOLO export uses a graph
object plus a separately addressable external-initializer object. The native
assembly path previously fetched only the graph, so an external-data role
could pass inline fixtures but fail when ONNX Runtime tried to load missing
weights. The signed root metadata now binds initializer name, byte length, and
raw-object digest; the C++ bridge fetches and stages `model.onnx.data`, and the
Python assembler verifies the recipe's normalized initializer digest after
loading the pair, normalizing exporter-specific external-data filenames at the
private staging boundary. Graph-only external assembly is rejected. This closed a
real implementation gap but did not close the signed-package, live-runner, or
qualification gates at that point. The publication-side gap described here was
closed by iteration 45; this historical text must not be read as the current
root-publisher state.

**Audit correction (iteration 45)**: The canonical artifact root API now accepts
the external initializer's Data name, raw digest, and byte length. The
`CanonicalCatalogEnsurer` validates an optional initializer payload and
publishes that object before the existing layer/root sequence, preserving the
root-last barrier. This closes the publication-side gap identified in A180-71;
the signed YOLO package, production offer trust, live runner, and qualification
gates remain open.

**Audit correction (iteration 46)**: The production ACK-offer trust boundary is
now explicit. Formal Y-A/Y-B/Y-N execution must verify each `ProviderOfferV3`
with the existing NDNSF Trust Schema and Provider identity/certificate path,
anchored by the candidate-bound `SPEC180_YOLO_OFFER_TRUST_ROOT`. The verifier
must bind the signer certificate to the advertised Provider and service and
verify the canonical offer digest, validity window, request/attempt, model,
graph, and boot epoch. A caller-provided HMAC key map remains fixture-only and
cannot close the production planning or qualification gate. This is a contract
clarification; concrete verifier wiring and the live ACK-to-Response run remain
open under T006/T009/T011.

**Audit correction (iteration 47)**: The existing NDN ACK transport is validated
by the configured Trust Schema before `OnRequestAck` processes the payload, but
the current `AckCandidate` projection carries only the ACK name and message;
it drops the validated packet signer/key-locator provenance. Consequently a
production V3 offer verifier cannot yet bind the inner offer's `signer_key_id`
to the authenticated Provider identity. T006 must extend the existing
ServiceUser/pybind candidate projection to carry non-secret signer identity,
certificate/key-locator reference, and validated ACK wire digest (or an
equivalent authenticated proof) from that same Data packet. Missing provenance
or a mismatch with the ACK/offer Provider MUST fail closed before planning.
This is an implementation boundary, not permission to introduce a second
trust root; the caller HMAC map remains fixture-only.

**Audit correction (iteration 48)**: The ACK provenance projection is now
implemented at the existing packet boundary. `ServiceUser` copies the
Trust-Schema-validated ACK Data's signer identity, typed KeyLocator reference,
and complete-wire SHA-256 digest into `AckSelectionCandidate`; pybind and the
Python facade preserve those fields, while direct legacy fixtures default them
to empty. This removes the projection gap without introducing a second trust
root. Production ProviderOfferV3 verification, mismatch negatives, and a live
ACK-to-Response trace remain open; until they pass, T006/T009/T011/T014 are
not complete and no qualification result is implied.

**Audit correction (iteration 49)**: The V3 configuration boundary now rejects
caller-supplied `ack_coverage_roles` and `ack_coverage_predicate`; the
registered ACK timeout is the only discovery-closure authority for the
qualification path. V2 compatibility hooks remain available. This focused
guard does not replace the required production verifier or live trace. The
model-neutral built-in sequential fixture also now declares its first and last
roles as input-ingress and result-egress owners so the stricter V3 ownership
validation does not break Spec170 compatibility tests; that fixture is not the
Spec180 YOLO adapter and provides no qualification evidence.

**Audit correction (iteration 50, historical)**: The candidate-bound Provider-offer policy
is now defined separately in
`contracts/provider-offer-trust-v1.md`. It delegates packet and certificate
authentication to the existing NDNSF Trust Schema/Provider identity verifier,
binds the projected ACK provenance to the signed `ProviderOfferV3`, and keeps
the caller HMAC map fixture-only. The contract makes the missing production
verifier boundary executable, but its presence is not implementation or
qualification evidence. That historical collection reported 105 passing tests
with 19 warnings; the current iteration-54 collection reports 114 passing
tests. No live ACK-to-Response, MiniNDN, SIF, or Tiger result is claimed.

**Implementation correction (iteration 51)**: `ProviderOfferTrustVerifier` now
implements the candidate policy and signature/provenance checks required before
a V3 planning view is constructed, while delegating packet authentication to
the existing Trust Schema verifier. This focused evidence does not claim live
certificate-chain verification, ACK-to-Response execution, or qualification;
T006 and T011 remain open until the maintained network runner exercises those
paths.

**Release-boundary correction (iteration 39)**: The fixed Slurm export is
`--export=NONE,<explicit SPEC180_* assignments>`; bare `--export=NONE` would
discard the candidate-bound values required by the job. The checked-in
`run-functional.sh` now verifies the fixed Apptainer 1.5.3 executable and SIF
digest, probes the case runner inside the candidate image, and enters the SIF
with `exec --nv --cleanenv --pwd /bundle` plus only the declared read-only model
inputs and evidence bind. The real in-image case runner is not present yet;
this boundary therefore still fails closed and does not constitute a local,
SIF, or Tiger execution result.

## User Scenarios & Testing

### User Story 1 - Plan YOLO from actual Provider offers (Priority: P1)

As an application developer, I submit a YOLO model, task, and input without a
Provider list or preselected split. NDNSF-DI first discovers willing Providers,
then chooses a safe YOLO execution plan and role assignment from the returned
capabilities.

**Why this priority**: This is the missing behavior. The existing YOLO example
uses a preplanned compatibility path, so it does not demonstrate ACK-driven
model placement.

**Independent Test**: Use one canonical YOLO model with two controlled ACK
capability sets. An atomic-capable-only set must select the one-role candidate;
a fully capable set must select the certified shared-backbone candidate. Both
must be selected after ACK closure and produce the registered output.

**Acceptance Scenarios**:

1. **Given** only one Provider can execute the whole model, **when** ACK
   collection closes, **then** the planner selects the certified atomic
   candidate and assigns it to that Provider.
2. **Given** four eligible Providers for the shared-backbone roles, **when** ACK
   collection closes, **then** the planner selects the certified branched
   candidate and assigns each complete role exactly once.
3. **Given** no certified candidate fits the returned capabilities, **when**
   planning runs, **then** the request fails before Selection without inventing
   a Provider, unsafe cut, or partial plan.
4. **Given** the same ACK snapshot and the same two certified candidates in a
   different catalogue order, **when** planning runs, **then** feasibility and
   the selected candidate are unchanged; no candidate inherits the first
   candidate's role requirements.

---

### User Story 2 - Assemble and execute the selected YOLO roles (Priority: P1)

As a selected Provider, I receive an authenticated role assignment, fetch only
the canonical model objects required for that role, verify them, assemble or
reuse the local executable role, and return outputs through the declared NDN
dependency graph.

**Why this priority**: ACK-driven placement is not real unless the selected plan
controls the production Provider path and the distributed result matches the
original model.

**Independent Test**: Starting from the canonical YOLO package, execute both
the atomic and shared-backbone candidates through production role assembly and
compare their final detections with the same full-model oracle.

**Acceptance Scenarios**:

1. **Given** a signed assignment bound to the request, closed ACK snapshot,
   model, graph, candidate, role, and Provider, **when** all referenced objects
   verify, **then** the Provider assembles or reuses exactly that role and
   publishes its declared outputs.
2. **Given** an altered assignment, unsafe cut, missing object, wrong digest, or
   wrong Provider identity, **when** preparation begins, **then** execution
   fails closed before unverified model computation.
3. **Given** a valid shared-backbone execution, **when** both DetectShard outputs reach
   the merge role, **then** the distributed detections match the registered
   full-model result within the declared numerical tolerance.
4. **Given** the registered YOLO image is larger than the bounded inline-input
   limit, **when** the request is submitted, **then** the Request carries only
   an authenticated encrypted repository reference and the candidate-declared
   input-ingress role
   fetches, decrypts, and verifies the exact input after Selection.

---

### User Story 3 - Qualify the YOLO path locally (Priority: P2)

As an operator, I can run the ACK-driven YOLO path through the same generic
orchestration and Provider runtime that will be sealed in the SIF before
creating an expensive deployment artifact.

**Why this priority**: Fast local gates must find semantic, security, routing,
and packaging-input defects before SIF construction or Tiger allocation.

**Independent Test**: From one unchanged source identity, the YOLO-relevant
unit/integration selectors and CPU/MiniNDN Y-A, Y-B, and registered critical
negative controls pass.

**Acceptance Scenarios**:

1. **Given** the completed implementation, **when** the code-aware convergence
   audit runs, **then** every requirement maps to a production entrypoint,
   runtime owner, focused regression, formal local case, and expected evidence.
2. **Given** audit `PASS`, **when** the local test ladder runs, **then** all
   registered YOLO cases pass from one unchanged source and effective
   configuration.
3. **Given** any behavior-affecting change after audit or local qualification,
   **when** promotion is attempted, **then** the affected evidence is invalidated
   and the process returns to the earliest controlling gate.

---

### User Story 4 - Run one real Tiger YOLO request (Priority: P3)

As a researcher, I can use one immutable runtime candidate to complete one
finite ACK-driven YOLO request on TigerCluster and obtain an auditable
functional verdict.

**Why this priority**: Real GPU execution is required to prove deployment
viability, but it must occur only after implementation and cheap validation are
complete.

**Independent Test**: One sealed runtime image completes one registered YOLO
Y-B request on one Tiger node. Four independent Provider processes own the
four selected roles, the three model roles use CUDA ONNX Runtime on the
allocated GPU, and all protocol, numerical, process, and cleanup oracles agree.

**Acceptance Scenarios**:

1. **Given** the sealed candidate and external canonical YOLO artifacts,
   **when** the registered YOLO job runs, **then** four distinct Provider
   processes execute the selected shared-backbone plan, the three model roles
   use CUDA ONNX Runtime on the one allocated GPU, the `Merge` role remains
   explicit on CPU, and the result matches the oracle.
2. **Given** the job has a protocol, numerical, runtime, child-exit, or cleanup
   failure, **when** closure runs, **then** the feature is reported as
   `UNQUALIFIED`; evidence from another candidate cannot fill the gap.
3. **Given** the Tiger functional slice passes, **when** the result is reported,
   **then** it is described as a one-node multi-Provider functional deployment,
   not as multi-GPU scaling or performance evidence.

### Edge Cases

- ACKs arrive late, duplicate an identity, advertise stale cache state, omit a
  required capability, or disagree with the selected runtime/model digest.
- The ACK snapshot supports an atomic candidate but not the preferred branched
  candidate.
- A graph looks syntactically splittable but its cut violates YOLO adapter
  semantics or omits a required output/initializer.
- Two roles are assigned to one Provider, one role is assigned twice, or a role
  is unassigned.
- A canonical object is missing, truncated, replayed from another model, or
  altered after the plan is sealed.
- A large invocation input is embedded in the Request, its repository reference
  is plaintext, or a Provider other than the candidate-declared input-ingress
  role tries to fetch
  or decrypt it.
- A warm YOLO request sees an incompatible cached role.
- An assembled-role cache entry has the right model digest but a different
  security domain, protection epoch, role kind, adapter ABI, or object set.
- Qwen continuation state is stale, absent, placed on the wrong Provider, or
  incompatible with the model/tokenizer/plan identity.
- A model-compute role silently falls back to CPU.
- A four-Provider YOLO process layout shares one physical GPU and is therefore
  only a process smoke, not distributed GPU qualification.
- Slurm or host infrastructure fails before workload entry; the candidate bytes
  remain unchanged.

### Registered Y-N matrix

`Y-N` is one inventory entry and one command, but its internal subcases are
fixed by the runner contract. The runner must not accept a caller-selected
subset or reorder them from ambient configuration.

| Subcase | Setup | Expected boundary | Outcome |
|---|---|---|---|
| `Y-N-O` | Reverse the signed catalogue entry order | Feasibility and selected candidate are unchanged | PASS control |
| `Y-N-C` | Remove `FullModel` and at least one required shared-candidate role capability from the closed ACK snapshot | Neither registered candidate is feasible; fail before Selection | Expected failure |
| `Y-N-P` | Alter or remove ACK signer/provenance or offer binding | Offer rejected before candidate feasibility | Expected failure |
| `Y-N-R` | Alter a component role into an invalid range/range role | Role-kind validation before artifact fetch/compute | Expected failure |
| `Y-N-I` | Invoke input fetch from a non-ingress role | `DI_INPUT_FETCH_ROLE_MISMATCH` before decrypt/fetch | Expected failure |
| `Y-N-E` | Use a stale or revoked protection epoch | Assignment/cache authorization fails before compute | Expected failure |
| `Y-N-L` | Inject a plaintext-bearing field into an emitted record | Redaction violation is detected before evidence acceptance; no terminal success is accepted | Expected failure |

For `Y-N-O`, the runner compares the canonical candidate/role decision with the
control ordering and requires equality. For every other subcase, the expected
failure must be observed at or before the listed boundary, with no terminal
success result. The aggregate case passes only when all seven subcases have
their expected outcomes, all child processes terminate cleanly, and no
plaintext or secret capability appears in logs or evidence.

A subcase MAY be reported PASS only when the runner observes the registered
rejection reason or marker at or before the listed boundary. An unrelated
runtime, repository, or strategy failure, a wrong lifecycle phase, or a
fabricated rejection MUST NOT count as the expected outcome (revision 124,
2026-09-05; regression `tests/python/test_spec180_negative_verdict.py`).

### Fixed MiniNDN capability profiles

The startup configuration is a capability allowlist, not a placement decision,
but it must still describe the intended case rather than a degenerate process
smoke. The runner therefore requires these exact Provider identity counts:

| Case | Provider identities | Capability-profile requirement |
|---|---:|---|
| `Y-A` | 1 | The sole identity advertises only `FullModel`. |
| `Y-B` | 4 | The identities have a distinct capability cover for `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`. |
| `Y-N` | 4 | The identities have both a distinct four-role shared-candidate cover and at least one `FullModel` capability. |

The distinct-cover check is only a preflight witness that the configured
profile can exercise the registered candidate. It records no role assignment
and cannot override the authenticated ACK snapshot or sealed plan. Overlapping
capability advertisements remain legal when a distinct cover exists; a profile
with one all-capable Provider and duplicate/unused identities is rejected
before NFD startup because it cannot demonstrate the intended multi-Provider
case. `Y-N` uses the same four identities for its order-control and negative
subcases; each subcase receives a fresh isolated state and evidence directory.

## Requirements

### Functional Requirements

- **FR-001** — **Canonical YOLO input.** Offline preparation MUST produce one
  immutable YOLO26n ONNX graph, external weights where applicable, graph/tensor
  metadata, preprocessing/postprocessing identity, and adapter-certified safe
  cut descriptions. When external initializers are used, the graph and
  initializer object MUST have separate content-addressed names, byte lengths,
  and raw-object digests; the Provider assembler MUST authenticate and stage
  both objects before ONNX loading. A graph-only fetch is invalid. The package
  manifest MUST bind the source checkpoint and digest, exporter dependency lock
  and command, ONNX opset, FP32 dtype, batch size 1, and static 640-by-640
  input. It MUST NOT contain a final Provider assignment.
- **FR-002** — **Certified candidate catalogue.** The YOLO adapter MUST inspect the
  canonical model and expose only verified execution candidates. The required
  catalogue contains an atomic one-role candidate and a shared-backbone
  candidate with complete `BackboneNeck`, `DetectShard0`, `DetectShard1`, and
  `Merge` roles. The two detection roles are shards partitioning the model's
  Detect-scale branches; they are not two independent model heads. Each
  candidate MUST declare exactly one `input_ingress_role` and one
  `result_egress_role`. A safe cut is a semantic graph contract, not a node
  position: each cut MUST be justified by the canonical role node sets, branch
  ownership, producer/consumer tensor names and shapes, per-role input/output
  interface, dependency edges, and an adapter-produced equivalence/oracle
  check. Percentile, fixed-index, or topological-prefix heuristics alone are
  not safe-cut evidence and MUST be rejected by the exporter/adapter. The
  catalogue MUST bind the complete node/tensor/interface proof for every
  candidate, not merely the names of boundary nodes. The catalogue manifest
  MUST carry a trusted signer key
  identity and signature covering the model, graph, candidate roles, safe cuts,
  ingress/egress roles, priorities, and merge semantics; an unsigned,
  unknown-key, or invalidly signed catalogue MUST fail before candidate
  enumeration. A candidate absent from this catalogue MUST NOT be selected.
  The trust-root record, covered-field set, and key/public-key digest MUST come
  from `contracts/catalogue-trust-root-v1.md` and the machine-readable
  `contracts/trust-root-registry-v1.json`; a profile or environment variable
  MUST NOT replace either record. An empty or `UNCONFIGURED` registry blocks
  T001 completion and all later qualification gates.
  This package catalogue is distinct from the runtime active snapshot. The
  package record certifies candidate semantics but does not authorize runtime
  artifact names; after ACK closure the User must resolve the signed
  `ndnsf-di-presplit-catalog-snapshot-v1` APP envelope, whose candidate-bound
  role/rank `artifactDataNames` refer to already published objects.
- **FR-003** — **Request-before-plan ordering.** The production application path
  MUST publish the generic request and close one authenticated ACK snapshot
  before enumerating candidates, evaluating feasibility, choosing a split,
  assigning Providers, preparing role artifacts, or committing Selection.
  Static canonical-package integrity checks MAY run before Request publication,
  but they MUST NOT enumerate request candidates or bind a Provider. Spec180
  closure is timeout-driven using the registered ACK window;
  caller-supplied `ack_coverage_roles` or another offline candidate/role
  predicate MUST NOT close the window early. For `REPO_REF`, evidence MUST show
  the authenticated input reference publication before `REQUEST_SENT`.
  Before `ACK_CLOSED`, the runtime may validate the signed catalogue as an
  opaque revision for safety (including its signature and package digest), but
  it MUST NOT expose candidate records or requirements to the planner, evaluate
  feasibility, or bind a Provider. Request-time candidate enumeration begins
  only after the closed snapshot is available. A preflight descriptor may
  record the immutable catalogue revision/digest for reproducibility; that
  descriptor is not a planner input or placement decision.
- **FR-004** — **ACK-owned planning input.** Placement MUST use the returned
  Provider identity, runtime compatibility, available memory, compute/device
  capability, role support, and compatible cache inventory. Caller-supplied
  role maps or offline Provider profiles MUST NOT be the qualification path.
  A runner-only process-start authorization allowlist may name identities and
  the roles they are permitted to advertise, but it is not a Provider list for
  the request and MUST NOT assign a role. Overlapping advertisements are
  allowed; only the closed authenticated ACK snapshot may provide the
  request-time candidates.
  Feasibility MUST be evaluated against each candidate's own complete role and
  dependency requirements; candidate ordering and any legacy global
  `required_roles` field MUST NOT change the decision. When more than one
  registered candidate is feasible, the adapter's signed candidate priority is
  the first tie-break (`shared-backbone-two-shard-v1` is preferred over
  `atomic-v1` only when its four-role requirements are fully met); a stable
  candidate-digest tie-break handles equal priorities. The priority and
  tie-break result MUST be included in placement evidence and the sealed plan.
  Each V3 offer MUST be authenticated by the existing NDNSF Trust Schema and
  Provider identity/certificate verifier anchored by the candidate-bound
  `SPEC180_YOLO_OFFER_TRUST_ROOT` as specified in
  `contracts/provider-offer-trust-v1.md`; the verifier MUST bind signer identity to
  the advertised Provider/service and canonical offer digest, request/attempt,
  model/graph, validity window, and boot epoch. A caller HMAC key map is
  test-fixture material only and MUST NOT qualify a request.
- **FR-005** — **Plan lineage.** The committed plan MUST bind request and attempt
  identities, ACK snapshot digest, model and graph digests, adapter and candidate
  digests, strategy identity, complete role-to-Provider map, dependencies, and
  artifact references. It MUST also bind the candidate's explicit
  `input_ingress_role` and `result_egress_role`; the shared candidate uses
  `BackboneNeck` and `Merge`, while the atomic candidate uses `FullModel` for
  both.
- **FR-006** — **One-to-one role ownership.** Every role MUST be assigned exactly
  once, every selected Provider MUST own exactly one complete role, and a
  logical role MUST NOT be spread across Providers. Cross-Provider tensor
  parallelism is not an exception and remains out of scope.
- **FR-007** — **Fail-closed feasibility.** If no certified candidate fits the
  closed ACK snapshot, planning MUST fail before Selection and MUST NOT weaken
  memory, runtime, role, security, or assignment constraints.
- **FR-008** — **Authenticated assembly.** Each role assignment MUST be signed and
  bound to the request, attempt, ACK snapshot, model, graph, candidate, plan,
  role kind, role, Provider, objects, security domain, protection epoch, and
  deadline. A qualification assignment MUST use a non-`plaintext-v1` protected
  epoch; the source-compatible `RoleAssemblySpec` default is not admissible for
  Spec180 local, SIF, or Tiger evidence. A mismatch MUST fail before execution.
  Protected canonical artifacts MUST be authorized through the inherited
  Spec170 `artifact-assembly-v1` grant flow: seal the plan core, authenticate
  a grant request to the configured policy authority, obtain
  Provider-recipient-encrypted signed grant Data, finalize the plan with grant
  references, then verify and unwrap inside the selected Provider (Python and
  native paths). HMAC scaffolding, a Python `repr` request digest, or a
  `plaintext-v1` role default MUST NOT satisfy the protected epoch.
- **FR-009** — **Canonical object verification.** Providers MUST fetch canonical
  objects by their declared NDN names and verify exact bytes and digests before
  assembling or reusing a role. For external-initializer ONNX, the object set
  is the graph plus the separately named initializer object; both names, sizes,
  and raw-object digests are required in the signed root metadata, while the
  recipe's normalized initializer digest is checked after ONNX loads the pair.
  A cache hit is advisory until the same identity is verified locally. Cache
  identity MUST include candidate, model, graph, adapter/assembler ABI, role
  kind, role, runtime, canonical object set, security domain, and protection
  epoch; every reuse MUST re-authorize the current request and reject a stale
  or revoked epoch.
- **FR-010** — **Model-neutral deployed runtime boundary.** The YOLO subject MUST
  use the generic NDNSF-DI orchestration, native Provider, canonical object
  transport, ONNX Runtime execution, security, and evidence mechanisms rather
  than a YOLO-specific network/runtime path. Deployed runtime MUST NOT import
  PyTorch, Transformers, or Ultralytics.
- **FR-011** — **YOLO numerical correctness.** Atomic and distributed YOLO results
  MUST use the same registered preprocessing, merge/postprocessing, and output
  comparison. The distributed output MUST match the full-model oracle within
  `atol=1e-3` and `rtol=1e-4`, with the same detection ordering rule.
- **FR-012** — **Qwen scope boundary.** Spec180 MUST preserve the named Spec175
  streaming handoff and MUST NOT change its prefill/decode, state,
  continuation, or terminal contracts. Qwen3.6-27B packaging, SIF execution,
  Tiger execution, and cross-model qualification are deferred and MUST NOT
  block or be claimed by the Spec180 YOLO functional verdict.
- **FR-013** — **Cheap-first validation.** Implementation and focused repair tests
  MUST finish before the design-code convergence audit. Complete suites and
  formal MiniNDN qualification require audit `PASS`; SIF creation requires the
  unchanged local qualification to pass.
- **FR-014** — **YOLO local qualification.** From one source identity,
  CPU/MiniNDN MUST execute Y-A, Y-B, and the registered critical Y-N controls
  through real NFD, NDN-SVS, security, ACK/Selection, NDN dependencies,
  terminal results, child exits, and cleanup.
- **FR-015** — **Immutable candidate.** Before SIF construction, upload, staging,
  or Slurm submission, a repository-owned closure gate MUST bind source,
  dependencies, build recipe, native/Python runtime, local test/replay harness,
  submit bundle, effective profiles, external model artifacts, workloads,
  oracles, and result schema. Rejected mutations MUST cause zero external side
  effects.
- **FR-016** — **One sealed runtime.** One locally built SIF hash MUST pass the
  exact-SIF Y-B replay and the Tiger YOLO job. Model artifacts remain external
  and content-addressed; Tiger MUST verify and execute the SIF rather than
  build, patch, or overlay it.
- **FR-017** — **Finite Tiger profile.** The sole Spec180 functional job is one
  node, one allocated RTX GPU, four independent Provider processes, and one
  cold Y-B request. `BackboneNeck`, `DetectShard0`, and `DetectShard1` use CUDA
  ONNX Runtime on that GPU; `Merge` remains an explicit CPU role.
- **FR-018** — **Hardware/runtime proof for Tiger.** Every model-compute role in
  the YOLO job MUST record its Provider process, role, CUDA ONNX Runtime
  provider, and physical device identity. The same physical GPU identity may
  appear for all three model roles in this functional profile. CPU model
  fallback is forbidden; the declared `Merge` postprocessing may run on CPU.
  Local Y-A/Y-B/Y-N records MUST identify their CPU backend and MUST NOT be
  presented as Tiger GPU evidence.
- **FR-019** — **Terminal evidence.** A job passes only when protocol oracles,
  numerical/token results, fresh result files, all child exit statuses, runtime
  provider evidence, and bounded cleanup agree under one candidate identity.
  Each oracle MUST be a structured, candidate-bound evidence record containing
  a repository-relative path, SHA-256 digest, schema/version, and the fields
  needed to verify its claim (including lifecycle records, request counts,
  device identities, and model/result references). A bare string such as
  `"protocolOracle": "PASS"` is insufficient; the validator MUST reject
  label-only or missing evidence records.
- **FR-020** — **Bounded recovery.** No changed-parameter diagnostic Tiger job is
  allowed. One byte-identical resubmission is permitted only for a recorded
  Slurm/host failure before workload entry; a source, model, profile, launcher,
  SIF, workload, or oracle change creates a new candidate and returns to its
  earliest invalidated gate.
- **FR-021** — **Functional claim only.** The feature MAY record elapsed times for
  diagnosis but MUST NOT claim throughput, latency, scaling, or performance
  superiority and MUST NOT add seeds, sweeps, warmup studies, or statistical
  comparisons.
- **FR-022** — **Bounded invocation-input transport.** `ApplicationInput` MUST
  support exactly one of two canonical modes: inline bytes no larger than 4096
  bytes, or a repository-backed `LargeDataReference` containing the immutable
  NDN name, object identifier, plaintext size, content digest,
  publication-manifest digest, encryption flag, input-schema digest, and
  protection epoch. The registered YOLO image MUST use
  the reference mode: the generic Request carries only the authenticated
  encrypted reference, and only the candidate-declared input-ingress role
  fetches, decrypts, and verifies the input after Selection. The selected role
  MUST use a repository primitive that verifies the reference's manifest/content
  digest and encrypted authorization; a name-only or size-only fetch is not
  sufficient. If the runtime cannot obtain that verification, it MUST fail
  closed with `DI_INPUT_REPO_DIGEST_UNVERIFIED`. The sealed
  dataflow MUST represent this as one `APPLICATION_INPUT` endpoint whose
  consumer is that ingress role; a non-ingress fetch fails closed with
  `DI_INPUT_FETCH_ROLE_MISMATCH`. The candidate-declared result-egress role is
  the only role permitted to publish the terminal result; a different role
  fails closed with `DI_TERMINAL_RESPONSE_ROLE_MISMATCH`. Input bytes and
  terminal results MUST use the existing NDNSF confidentiality/authorization
  path and MUST NOT appear in logs, plans, ACKs, profiles, or evidence.
- **FR-023** — **Role-kind-specific assembly.** `RoleAssemblySpec` validation MUST
  distinguish `PIPELINE_RANGE`/rank roles from `COMPONENT_SET`. Range roles
  require a valid non-empty layer interval; a YOLO component role requires a
  canonical non-empty, sorted, duplicate-free node set and MUST encode the
  reserved empty-range sentinel `layer_begin=0, layer_end=0`, not a fabricated
  Transformer layer range. Python wire validation, native parsing, bindings,
  assembler behavior, and focused negative tests MUST enforce the same rule.
- **FR-024** — **Registered local-suite inventory.** The local qualification
  manifest MUST enumerate the exact C++ binaries/suite selectors, Python test
  selectors, and MiniNDN case entrypoints that form the Spec180 gate. Each item
  MUST run in its own supervised child and record command, source/candidate
  identity, exit status, signal, timeout, and cleanup result. “Complete suite”
  MUST NOT be interpreted as an unrecorded ad hoc subset.

- **FR-025** — **Candidate-bound YOLO runner.** The Y-A/Y-B/Y-N MiniNDN
  entrypoint MUST use the runner contract in
  `contracts/yolo-minindn-runner-v1.md`. It MUST reject missing or ambient
  candidate inputs before starting NFD or any Provider, MUST NOT use the
  legacy `/Stage/...` policy as qualification authority, and MUST emit the
  registered case marker only after the complete ACK-to-Response, numerical,
  security, redaction, child-exit, and cleanup oracles agree. The runner MUST
  reuse the existing MiniNDN setup and teardown helpers from
  `Experiments/NDNSF_DI_Yolo2x2_Minindn.py` (including NFD/SVS startup,
  routing, keychain installation, and process supervision), adapting only
  candidate-specific configuration. It MUST NOT call that legacy runner's
  deployment-first `main()` or copy its output into a Spec180 result.
  Lifecycle events
  MUST use a closed allowlist of milestone-specific non-secret fields and
  recursively reject arbitrary payload/content/token/byte fields even when
  their values do not match a secret-name pattern. The local case entrypoint
  and the in-image `scripts/run_spec180_case.py` dispatch shim MUST resolve to
  the same candidate-bound case implementation; neither may silently invoke a
  deployment-first oracle. A caller HMAC offer-key map is not a qualification
  trust root. The runner MUST start the process vector in three barriers:
  `control` (Controller and repository), readiness for both, `providers` (all
  authorized capability Providers), readiness for every Provider, then the
  catalogue publication barrier and finally `user`. `start_processes()` MUST
  reject an unknown or repeated phase and resolve all phase nodes before
  creating its first child; each phase MUST reject an uncompleted predecessor.
  The runner MUST record the verified catalogue publication receipt (exact
  Data name, signer, and digest) through `mark_catalogue_published(...)` before
  starting `user`; readiness markers are mandatory and a child exit
  or timeout before its marker is a fail-closed case result. After the fixed
  Controller/repository/Provider readiness markers
  are observed, the runner orchestration MUST publish the exact signed
  catalogue APP Data record through the maintained
  `ServiceUser.publish_signed_app_data` path. The maintained User process
  launched by that runner MUST publish the encrypted YOLO `REPO_REF` through
  `APPClient.publish_application_input_reference()` before `REQUEST_SENT`.
  The existing controller artifact-deployment manifest, an offline snapshot
  file, or an ambient local path is not catalogue or input publication
  evidence. A publication failure MUST stop the case before a request is sent
  and MUST be recorded as an unqualified boundary failure.

### Key Entities

- **Canonical YOLO package**: Immutable model graph, weights, graph/tensor
  metadata, preprocessing/postprocessing identity, and safe cut catalogue.
- **Invocation input**: Either bounded inline bytes or an authenticated
  repository-backed encrypted reference; the registered YOLO workload always
  uses the latter and binds its schema, digest, size, and protection epoch.
- **Provider capability ACK**: Authenticated request-scoped offer describing
  runtime, memory, compute/device, role support, and compatible cached objects.
- **Closed ACK snapshot**: The immutable set and digest used for one placement
  decision.
- **Safe candidate**: Adapter-certified role/dependency/artifact topology with
  explicit feasibility requirements, exactly one input-ingress role and one
  result-egress role, and no Provider binding.
- **Role assembly assignment**: Signed request/plan/Provider-bound instruction
  describing exactly which canonical objects form one complete role.
- **Assembled-role cache entry**: Verified local executable role keyed by model,
  graph, candidate, adapter/assembler ABI, role kind, object set, runtime,
  security domain, and protection epoch; it never substitutes for current
  request authorization.
- **Qualification candidate**: Immutable identity across source, runtime image,
  harnesses, profiles, model artifacts, workloads, and evidence contract.
- **Workload record**: One registered model case, request sequence, expected
  oracle, effective hardware mapping, and terminal result.
- **Closure verdict**: Candidate-bound `FUNCTIONAL_PASS` or `UNQUALIFIED` with
  the first failing layer and preserved evidence.

## Success Criteria

### Measurable Outcomes

- **SC-001**. In controlled local planning tests, 100% of atomic-only ACK
  snapshots select the atomic candidate and 100% of fully capable snapshots
  select the shared-backbone candidate, always after ACK closure.
- **SC-002**. All registered malformed, stale, unsafe-cut, missing-object,
  wrong-digest, wrong-Provider, duplicate-role, and insufficient-capability
  cases fail before unverified execution or Selection, as applicable. Swapping
  candidate catalogue order produces the same feasible decision.
- **SC-003**. Atomic, local distributed, and Tiger distributed YOLO results all
  match the same full-model oracle within `atol=1e-3` and `rtol=1e-4`.
- **SC-004**. The source-bound local gate completes the registered
  YOLO-relevant unit/integration selectors and Y-A/Y-B/Y-N MiniNDN cases with
  zero unexpected failures, all child statuses collected, and zero owned
  survivors.
- **SC-005**. One SIF hash passes import/ABI/library/runtime checks and executes
  the exact-SIF YOLO Y-B replay without a source or package overlay.
- **SC-006**. The YOLO Tiger job completes 1/1 registered request with four
  unique role owners, CUDA ONNX Runtime on the allocated GPU for all three
  model roles, no CPU model fallback, a matching result, and clean termination.
- **SC-007**. The final report makes no Qwen, multi-GPU, warm-cache,
  throughput, latency, scaling, or performance claim from the Spec180 result.
- **SC-008**. A single closure record maps 100% of the active Spec180
  requirements to code, tests, the one Tiger job, and candidate-bound evidence
  and emits exactly one functional verdict without combining candidates. Every
  referenced oracle is present, digest-verified, schema-checked, and tied to
  the same candidate; a manifest containing only PASS labels cannot satisfy
  closure.
- **SC-009**. The registered YOLO input is never embedded in Request/ACK/plan or
  evidence, is fetched only by the candidate-declared input-ingress role through
  its encrypted repository reference, and all component/range-role plus cache
  security-epoch positive and negative cases agree in Python, native, and
  MiniNDN execution.

## Assumptions

- Spec175 will provide `LOCAL_FUNCTIONAL_PASS` at a named baseline commit before
  Spec180 formal qualification begins. Spec180 records its delta from that
  baseline and re-audits and re-tests the complete current candidate; the
  inherited PASS is not substituted as evidence for changed code. Independent
  YOLO implementation may proceed before the handoff closes.
- `yolo26n.pt` is used only by the offline preparation environment; the
  qualification subject is its frozen canonical ONNX package. The two current
  workspace copies are byte-identical at 5,544,453 bytes with SHA-256
  `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`;
  T004 must reject another checkpoint rather than silently changing the subject.
- Spec175 Qwen material is a preserved compatibility handoff only. It is not a
  Spec180 local or Tiger workload and cannot satisfy or block the YOLO verdict.
- Tiger can allocate one node with one RTX GPU. The three YOLO model Provider
  processes share that GPU; the `Merge` Provider is a separate CPU process.
- The YOLO model artifacts are staged outside the SIF and identified by content
  digest. Their manifest is verified against the candidate-bound experiment
  artifact authority before staging; a profile or ambient environment variable
  cannot provide artifact authority.
- Spec180 qualification uses a 4096-byte inline-input ceiling; the registered
  640-by-640 YOLO image is always published as an encrypted repository object,
  regardless of its encoded byte size.

## Out of Scope

- New generic streaming, KV-cache, conversation, or Qwen semantics.
- Qwen3.6-27B local/SIF/Tiger execution, cross-model qualification, a second
  warm YOLO request, and distinct-GPU YOLO placement.
- Cross-Provider tensor parallelism, role replication, model-state migration,
  dynamic role count beyond the two certified YOLO candidates, or a new
  placement strategy.
- Training, fine-tuning, model export on Tiger, or PyTorch/Transformers/
  Ultralytics in the deployed runtime.
- Multi-node qualification, failure-recovery campaigns, performance tuning,
  throughput/latency claims, scaling studies, seed matrices, or paper-ready
  benchmark conclusions.
- Reusing a Spec175 SIF, job, or partial trace as Spec180 acceptance evidence.
