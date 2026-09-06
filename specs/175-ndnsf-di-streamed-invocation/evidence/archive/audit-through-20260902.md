# Archived Spec 175 Audit Through 2026-09-02

> Historical audit record only. Use `../../audit.md` for the current verdict.

**Audit date**: 2026-09-02
**Scope**: `spec.md`, `plan.md`, `tasks.md`, `research.md`, `data-model.md`,
`quickstart.md`, `experiment-plan.md`, `traceability.md`, all versioned
contracts, and current implementation seams inspected through CodeGraph.  
**Document verdict**: **PASS after the 2026-09-01 Tiger-baseline correction**
**Design-to-code convergence verdict**: **PASS for the T024 submission
boundary; runtime/protocol convergence remains PASS**
**Implementation/promotion verdict**: **PASS through post-correction G3
host/CPU qualification; BLOCK at T023 until exact-SIF replay closes**

## Current closure checkpoint — post-correction G0--G3

The T024 submission-boundary correction is now included in one fresh source
seal. G0, G1, and G2 pass under
`results/spec175/g0/source-seal-g2-probe-fixed-20260901T221454Z.json` (source
revision `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`); G1 records 368 passed,
zero failed, and one explicit skip, while G2 records 38/38 registered I01--I20
results using the checked-in `build/integration-tests` Boost.Test registry.
The strict G3 manifest
`results/spec175/g3/qualification-manifest-post-profile-20260901T221454Z.json`
records 42/42 independent M01--M14 host/CPU processes with four Providers,
disabled admission control, the frozen topology, and complete terminal and
conversation evidence. These records share the same source seal and close
T020 and T022. M13/r2 startup/route failures were retained as setup diagnostics
and excluded; a same-tuple retry after scoped Mininet cleanup passed and only
its final three repetitions are included.

The current T023 candidate has been built exactly once from this sealed subject:
SIF digest `sha256:6cbac977d085a220fc40c0470a54e5fa4874a2386b870e2ec1db6039202a7ceb`.
Its in-SIF and host-substrate preflights passed, and the immutable exact-SIF
M01--M14 replay reached 29/42 PASS before the 7200-second campaign watchdog
expired. The first incomplete row was M02-r3: repository publication and
three Provider readiness records were present, but no fourth Provider
readiness/case-result was produced. Therefore this is an incomplete G4
harness/replay result, not a 29/42 qualification; the remaining 13 rows are
`NOT_RUN` and no Tiger job may be substituted. No historical SIF, old G4
replay, or Tiger job may be substituted; Tiger remains unauthorized until a
fresh complete replay or a documented identical retry closes the matrix.
The retained run record is
[`evidence/t023-g4-timeout-20260902.md`](evidence/t023-g4-timeout-20260902.md).

The tracked replay driver was then corrected to own the timeout and terminal
boundary: a 900-second per-case watchdog, a 14,400-second campaign bound,
fresh process groups/output directories, four-Provider readiness evidence,
bounded descendant teardown, and explicit `firstIncomplete`/`notRunEntries`
on interruption. Focused replay-driver tests pass. This is a behavior-bearing
host-harness change, so the post-contract SIF and its 29/42 replay are now
diagnostic only; the earliest restart gate is a new source seal followed by
G0--G3 and a new exact SIF. No Tiger submission is authorized from the old
candidate.

The first post-correction reseal attempt was blocked at G1 because the linked
NDN-SVS library did not match the Experimental header. The pair is now aligned,
the production catch-up path has been repaired, and its focused regression
passes in three independent processes. This closes the diagnosed parity/path
finding, not the full reseal. The cancellation test passed alone after one
full-suite intermittent failure. Evidence:
[`evidence/t020-g1-svs-api-parity-20260902.md`](evidence/t020-g1-svs-api-parity-20260902.md).
The current frontier is T043's finite Tiger-use-case/G5U/G4-sentinel correction,
followed by one new source seal and G0--G3 run; no old G0--G4 or Tiger claim may
be reused.

## Control-plane synchronization checkpoint — 2026-09-02

The one-script/one-profile/run-record rule is now a single maintained contract
at [`contracts/tiger-experiment-profile-v1.md`](contracts/tiger-experiment-profile-v1.md),
referenced by the Spec, plan, quickstart, and iTiger skill. It explicitly binds
fixed launch fields, the small allowlist of run-record deltas, the requirement
that every exported value have a tracked consumer, zero-side-effect mutation
preflights, and the byte-identical retry boundary. This removes the prior
documentation split that let an operator reconstruct a near-duplicate command
from otherwise correct scripts. The change is control-plane documentation
only; it does not alter the sealed source or authorize T023.

The contract now includes a normative registered-parameter table. It makes the
small allowed delta explicit (candidate/SIF/run/output identity, model identity
for model gates, stage-device IDs for G5, and manifest-listed case/seed rows
for local G3) and labels Provider/GPU counts, Tiger seeds, resources, `cwd`,
routes, timeouts, identities, helpers, and launch order as fixed. This is the
missing operational boundary that previously allowed a supposedly identical
experiment to drift while retaining the same informal job description.

The replay also exposed a matrix-isolation gap: a campaign-wide watchdog can
leave a later case between Provider startup and its terminal case-result. The
contract now requires a per-case watchdog, all-four-Provider readiness, and
teardown evidence before advancing; a global deadline must explicitly mark
the first incomplete case and all later rows `NOT_RUN`.

The broader process finding is now explicit in the contract: a single
successful script is not a reusable subject unless its source, profile,
configuration tree, runtime, and evidence tuple are unchanged. Future matrix
dimensions must be declared as profile axes. Run-level rows may reuse a sealed
candidate; subject-level changes such as coverage, speed, timeout, topology,
resource, or model changes require a new profile digest and lower-gate
qualification. This preserves one launcher/configuration while preventing
ambient or ignored parameters from creating an unobserved experiment.

## Why Spec175 is not complete yet

The canonical object/transport wiring gap is repaired and its pre-profile
source passed G0--G3. A later audit found a separate release-boundary mismatch:
T024 claimed a historical-success delta checker, but the production submit path
only checked candidate/checklist status, file existence, and hashes. It did not
compare the fully rendered Tiger command/configuration with one proven D0/r23
profile, restrict differences to gate-specific parameters, or prevent ambient
`--export=ALL` values. Thus repeated Tiger errors were possible even though the
same general scripts and checklists existed. That defect is now repaired by the
checked-in profile/runner, field-level allowlist, explicit `--export=NONE`,
exact validated/submitted argv check, and mutation tests; the old paragraph is
retained as incident history, not as the current implementation status.

The finite qualification frontier is now:

```text
T042 runtime code-aware PASS (closed)
  -> T024 proven-profile submission boundary (closed)
  -> T020/T022 fresh seal and host matrix (reopened; G1 currently blocked)
  -> T023/T025/T026/T034/T027/T028
```

## T024 implementation re-audit — 2026-09-01

The production boundary now matches the requested “one script, one
configuration, registered parameter deltas only” rule. The checked-in profile
and run record are the sole input to the submitter; the profile hashes the
helper/job closure and fixes the Apptainer, bundle `cwd`, export mode, identity,
mount, readiness, resource, and terminal contracts. The semantic renderer
rejects unknown or unsafe fields and changed helper/job bytes, records only
allowlisted parameter changes, and emits `proven-baseline-exact-delta=PASS`.
Provider/GPU counts, seed, and Slurm resources are fixed per gate because no
checked-in consumer reads ambient overrides; only the stage-readiness
device mapping is currently a registered runtime delta.
`submit_profile.py` re-renders before allocation and requires the exact same
argv, sealed environment, profile digest, and effective-configuration digest
that were written by the dry run. The scheduler receives `--export=NONE,...`;
the old ambient diagnostic wrapper is disabled.

Code-aware evidence: [`submit.sh`](../../packaging/ndnsf-di-container/jobs/spec175/submit.sh:4-18),
[`spec175_tiger_profile.py`](../../packaging/ndnsf-di-container/lib/spec175_tiger_profile.py:332-403),
and [`submit_profile.py`](../../packaging/ndnsf-di-container/jobs/spec175/submit_profile.py:137-205).
Focused verification passes 43 tests, the operator checklist self-test passes
6/6, the profile renders against the real five-gate job tree, and Python/shell
syntax checks pass. The subsequent same-seal G0--G3 chain was the prior local
closure; it is now historical because the replay-driver correction changed the
sealed host subject. T020/T022 are reopened, and these implementation checks
still do not qualify a SIF/Tiger candidate.

Exactly **34/42 tasks are closed and 8 remain open**. The unchecked task IDs are
`T020`, `T022`, `T023`, `T025`, `T026`, `T034`, `T027`, and `T028`. T024 remains
closed at its implementation boundary; the prior T020/T022 local qualification
is historical and remains diagnostic only.

The focused canonical-transport and Spec175 contract checks pass `333 tests`
with one explicit skip. The pre-profile G0--G3 rerun adds G1 `358 passed, 1
skipped`, G2's 20-case/three-repeat native integration result, and G3's 42/42
strict host/CPU matrix. The later candidate SIF
`sha256:b0f6a502e6646d2ad9054c95e29a50f2d1abd9f6b2e860fb2c93fbb00eadd992`
passed build/preflight and an exact-SIF M01 smoke. Its full replay was stopped
after 24/42 results once this audit proved that T024 required a tracked source
correction. No G4 aggregate or Tiger qualification is claimed.

## Historical qualification checkpoint — 2026-09-01 v5

The current source-bound local gates are not the reason for the remaining
failure: G0, G1, G2, and strict G3 all pass for the v5 source seal. One exact
SIF was then built with local Apptainer 1.5.3 and passed both the build-record
validator and the in-SIF runtime preflight. Its digest is
`sha256:dbf6bf487ec28025adbce248dba5aed7bd1160265fe6e8b35a2d7726524c3194`;
the embedded source seal is
`sha256:e5b537bd219aac3b41fa350a90400250e455830008747cfe84b0ad6c4d738948`.

The exact-SIF G4 replay is nevertheless **BLOCKED**. M01 reached all expected
control stages (Controller/repository/User/Provider startup, route probe, four
ACKs, Selection, and ordinary Provider Responses), then failed on the first
canonical model-object fetch after Selection. Every stage Provider reported
`nacabe.Consumer: Data fetch error: Nack Error`; the User reported a code-17
large-data authorization/decryption failure for a canonical
`/ndnsf-di/.../OBJECT/...` name. A second diagnostic replay supplied an explicit
`NDNSF_CONFIG` file and reproduced the same Nack, so the missing config file is
not the sufficient cause. The retained evidence is
[`evidence/t023-g4-failure-20260901-canonical-object-nack.md`](evidence/t023-g4-failure-20260901-canonical-object-nack.md).

The controlling discrepancy was a publication/identity mismatch: the V3
projection requested a canonical `/ndnsf-di` object, while the tiny repository
bootstrap published stage bytes only under repository transport names. The
source repair described in
[`evidence/design-code-audit-20260901-canonical-transport.md`](evidence/design-code-audit-20260901-canonical-transport.md)
now separates those names and publishes the source/root through the authorized
encrypted path. This does not retroactively repair the v5 image: T023 remains
open, the v5 SIF is diagnostic only, and no Tiger submission is permitted.

## T020 reseal checkpoint — 2026-09-01

The pre-transport-repair source-bound local gate was complete. G0, G1, and G2
reported `PASS` for source seal `20260901-v3-boundary-fix`; G1 recorded 354
passed, zero failed, one explicit unconfigured-real-model skip, and exit 0;
G2 recorded all I01--I20 with three healthy repetitions and no missing cases.
Because the canonical transport repair changes the production publication and
Selection path, those manifests are now historical and must not authorize a
new SIF. The post-edit G0 manifest was the stable record for that earlier
subject:
`results/spec175/g0/qualification-manifest-20260901-v3-boundary-fix.json`.
The detailed evidence and manifest hashes are in
[`evidence/t020-v3-boundary-fix-20260901.md`](evidence/t020-v3-boundary-fix-20260901.md).
This closed T020 only for the earlier subject. T022/G3 likewise closed on a
42-process host/CPU matrix against that same pre-repair seal; all such G3/SIF/
Tiger records remain excluded for the repaired source. The historical manifest
is
`results/spec175/g3/qualification-manifest-20260901-v3-boundary-fix.json` and
records 42/42 PASS.

The detailed controlling finding list is preserved in the historical
[`evidence/design-code-audit-20260831.md`](evidence/design-code-audit-20260831.md).
The current source status is defined by the fresh report below; earlier
statements about T020/T022/T031 or local G3 are retained only as historical
audit chronology.

The fresh code-aware report is
[`evidence/design-code-audit-20260901-canonical-transport.md`](evidence/design-code-audit-20260901-canonical-transport.md).

## Repair checkpoint — 2026-09-01

The current repair pass removed two false or misleading completion signals:

1. The native assembly bridge now normalizes the C++ wire spelling before
   constructing the Python `RoleAssemblySpec`/recipe, compares concrete shape
   dimensions semantically, and is linked into the real integration target.
   The focused helper/contract checks pass (`17 passed`), and the post-Selection
   production handler test passes after a clean integration rebuild. A new
   native fixture also runs the assignment-bound root, separately fetched source,
   real helper, signed manifest, content-addressed cache reuse, and tampered
   source rejection. That fixture exposed and fixed an extra JSON closing brace
   and an uppercase `ndn-cxx` digest representation at the Python boundary.
   T038 and T039 now have focused closure records covering the direct
   `CollaborationContext` path, registered 1/2/4-Provider fresh processes,
   C++ ORT load/warmup, launcher rejection mutations, native terminal text,
   authenticated sampling, and empty-delta/digest-only mutations.
2. The deterministic CPU fixture builder now generates the standalone ASCII and
   UTF-8 tokenizer files consumed by the native decoder. Its legacy model
   manifest remains scoped to the ONNX/prompt/tokenizer oracle, so the host-gate
   fixture digest is unchanged. The complete Spec175 Python contract set passes
   `321 passed, 1 skipped`; this is implementation evidence, not G0--G4 or
   Tiger qualification.

The expected-negative G0 rerun currently reports only
`DIRTY_INPUT_TREE` (143 in-scope paths). This is the correct fail-closed result
for the intentionally dirty worktree; it means no source-sealed candidate has
yet been created, not that the implementation convergence is incomplete.

## Mandatory pre-test gate

The design-to-code convergence review is a hard prerequisite for every formal
validation layer. Before a complete unit/integration suite, MiniNDN matrix,
benchmark, SIF replay, or Tiger job can be counted as evidence, the active
specification, plan, contracts, effective configuration, production entry
points, runtime wiring, security checks, terminal ownership, cleanup, logging,
and evidence schema must be traced against the current source. Each discrepancy
must be recorded with an owner and focused regression, repaired or explicitly
accepted as a blocking limitation, and re-audited. Only a fresh `PASS` in
[`../../.specify/memory/design-code-convergence.md`](../../.specify/memory/design-code-convergence.md)
unlocks formal validation. The fresh T042 re-audit now reports `PASS`; existing
full-suite, MiniNDN, SIF, and Tiger results remain historical for their old
source identities and must not be reused as the new candidate.
No formal test is accepted as current evidence unless it binds that fresh
source identity and passes this gate.

The required order is therefore: freeze the design and effective
configuration; inspect the real production path; repair every discrepancy;
run only the focused regressions needed to close those findings; re-audit; and
obtain a fresh `PASS`. That fresh `PASS` is now recorded below. A broad test
run must still bind a new exact source/configuration identity; it cannot reuse
an older manifest or support a paper claim for the repaired source.

The operator sign-off is the complete **Required sign-off checklist** in
[`../../.specify/memory/design-code-convergence.md`](../../.specify/memory/design-code-convergence.md).
An unchecked item remains `BLOCK` even when an individual command exits
successfully.

**Operator decision recorded 2026-09-01:** code/design distance must be
reviewed and repaired before a test result can be accepted. The per-run
sign-off card in [`docs/NDNSF-DI-runtime-workflow.md`](../../docs/NDNSF-DI-runtime-workflow.md)
is now part of the evidence record, and every behavior-affecting change
invalidates the prior audit identity. This prevents a passing isolated test,
stale binary, compatibility fixture, or mismatched launcher from being used
to qualify the intended production design.

## Completion-graph audit — 2026-09-01

The feature remained open partly because production work is genuinely
unfinished, but the task graph itself also made completion impossible. This is
a document defect, not evidence that MiniNDN, Apptainer, or Tiger must be run
again immediately.

| Finding | Severity | Evidence | Correction |
|---|---|---|---|
| C175-01: implementation and qualification formed dependency cycles | HIGH | T038 required MiniNDN although T022 was blocked on T042; T040 required a real CUDA process although Tiger was blocked on T042; T041 waited for T042 although T042 required T041 | Each implementation task now closes on a focused production-path test; T020--T027 own environment execution and measurement. |
| C175-02: candidate-gate implementation waited for the gates it must protect | HIGH | T035/T036 were placed before T020 but historical wording kept them open until fresh G0--G4 evidence | T035/T036 now close on fail-closed synthetic/live-probe/terminal focused tests, before T042 and G0. |
| C175-03: status summaries contradicted task checkboxes | HIGH | The qualification section said T020--T022 were closed while all three current checkboxes were open; the BLOCK summary omitted open implementation tasks | The current frontier now explicitly identifies T020/T022 as the next qualification tasks and T024 as closed; T035--T042 are closed at their implementation/convergence boundaries, and older closure text is historical only. |
| C175-04: completion scope could grow after every checkpoint | HIGH | The feature reached 82 FR, 21 SC, and 42 tasks, with 13 reopen references and implementation, deployment, performance, and conversation additions sharing one completion state | T043 is the sole audit-required correction that fixes the finite Tiger result matrix; after it closes, the 82-FR/21-SC/T001--T043 scope is frozen and new features or optimizations require a follow-up Spec. |
| C175-05: performance target could be read as an implementation blocker | MEDIUM | G7 defines a 20 token/s threshold but the feature also defines `FUNCTIONAL_PASS_PERFORMANCE_MISS` | A correct registered G7 measurement closes T027 with either verdict; only functional/correctness failure blocks release closure. |
| C175-06: task history obscures the executable frontier | MEDIUM | `tasks.md` contains many superseded checkpoint narratives alongside current acceptance language | The new frontier table and non-circular closure rules are normative; historical checkpoint paragraphs remain diagnostic evidence only and new history should be written under `evidence/`. |
| C175-07: T024 claimed a proven-baseline delta checker that production did not implement | HIGH (closed 2026-09-01) | The former `submit.sh` accepted ambient values and used `sbatch --export=ALL`; the former checklist had no semantic D0/r23 profile or gate allowlist | Implemented `proven-tiger-profile.json`, `spec175_tiger_profile.py`, the three-argument `submit.sh`/`submit_profile.py` boundary, explicit `--export=NONE`, registered delta reporting, and mutation tests. T020/T022 remain reopened because this tracked correction requires a fresh source seal. Evidence: `evidence/t024-proven-tiger-profile-20260901.md`. |

The corrected finite path is:

```text
T042 (runtime convergence PASS)
  -> T024 -> T043 -> T020 -> T022 -> T023
  -> T025 (G4T/G5U/G5) -> T026 -> T034 -> T027 -> T028
```

T037, T021, T029--T030, and T032--T033 remain closed at their explicitly
named implementation boundaries. A behavior change to one of those owners may
invalidate its closure; a later candidate or environment failure alone does
not reopen an already verified implementation task.

## T031 repair checkpoint — 2026-09-01

The code-aware finding that had kept T031 open is repaired. The native
`ConversationStateStore` no longer exposes an adapter-owned entry through its
serialized `lookup()` path. `NativeConversationStateHandleV1` carries only the
binding and logical byte count; the ONNX adapter owns the real ORT/CUDA/host
state and now implements promotion, restore, D2H pause, asynchronous H2D
prefetch with tracked raw allocations, generation-fenced cancellation, and
release. The CPU path remains the explicit serialized TensorBundle fallback.

The new instrumented production-runtime regression observes every ownership
callback and verifies that a resumed request reaches the adapter without a
`__ndnsf_provider_decode_state` TensorBundle. The ORT-enabled unit target
compiles/links 104/104, the transfer regression passes, and all six existing
conversation-state regressions pass. See
[`evidence/t031-adapter-state-transfer-20260901.md`](evidence/t031-adapter-state-transfer-20260901.md).

This closed T031's implementation boundary.  At that checkpoint T042 still
had to re-run the design-to-code trace; that convergence audit has since
passed, and T034/G6C still owns physical CUDA copy measurement on the final
candidate.

## Re-audit correction after the Experimental checkpoint

The 2026-08-27 checkpoint exposed two reproducibility defects that invalidate
the earlier promotion sequence without invalidating its diagnostic results:

1. the repository-owned Spec175 contract gate, Tiger checklist validator, and
   their focused tests were not included in the pushed checkpoint; and
2. the newest passing G1/G2 manifests were generated after the source seal used
   by the 42/42 G3 matrix. G1/G2 and G3 therefore describe different subjects.

Those gate files are restored to the versioned subject. A later G3 attempt then
exposed an independent repository-startup race: the publisher could issue its
first Store request before the Repo Provider installed the Store handler and
permission. Revision `f5f2cab9` adds a one-shot, token-checked publication
barrier while preserving concurrent DKEY bootstrap. The focused M07 process and
195 Spec175 Python tests pass. T020 has reclosed on that revision with clean G0,
native unit PASS, Python 222/222, and G2 38/38 bound to one source seal. The
fresh same-seal G3 matrix then passed all 42 independent M01--M14 processes,
including M09 and the M11--M14 conversation evidence. The old G3 matrix remains
historical regression evidence; only the new manifest authorizes T023.

## Executive finding

Spec175 now contains a coherent end-to-end design for both the original
request-scoped token stream and the newly requested multi-turn conversation
path. The conversation design is additive: callers without conversation options
retain the current full-context behavior. A later turn uses a stable
conversation identity but fresh Request/plan/Selection/generation authority,
an opaque authenticated parent checkpoint, protected User-side transcript
state, exact same Provider-role placement, Provider-local role state, delta
prefill, and the existing automatic autoregressive decode loop.

The current documents and focused repair checks pass the implementation
convergence gate.  The feature is still not complete or promotable because the
exact-SIF G4 gate is blocked; the current source exposes the additive
conversation API, protected checkpoint transaction, Provider-local state
stores, the native receipt/control seam, and M11--M14 host-gate entry points.
The following qualification boundaries must stay visible:

1. V3 Selection now carries a compact role-local conversation reference and
   `NativeProviderHandler` resolves it against Provider-owned state before
   setting `NativeEpochCoordinatorConfig::conversationStateBinding`. The
   handler now stages the finalized role state, emits a Provider-authored
   receipt through the existing signed/encrypted collaboration path, and waits
   for an exact User COMMIT/ROLLBACK control. The User validates the complete
   receipt set and emits one role-bound control per selected Provider. The
   host/CPU implementation boundary is now exercised by T022's repeated
   four-Provider M11--M14 matrix. The remaining gap is exact-SIF/GPU/Tiger
   qualification; and
2. CUDA state-output/input mapping and adapter-owned causal-position
   materialization are fixed and regression-tested.  The exact Qwen graph and
   physical CUDA residency/transfer evidence remain unqualified and belong to
   T025/T026/T034; and
3. `ConversationCoordinator` previously fell back to one public deterministic
   test HMAC key. Production `APPClient` now derives a purpose- and
   identity-separated checkpoint-authentication key ring from the
   owner-injected `RuntimeJournal` key, supports bounded previous-key
   verification, and rejects a coordinator with neither key source. This local
   checkpoint MAC does not substitute for Provider-signed and
   requester-encrypted Ready/receipt Data required by T030--T031; and
4. The M11--M14 probe still has a test-only emulation mode for local state
   accounting, but formal use is fail-closed. Current host/CPU checkpoints
   now exercise real independent Providers for receipts, promotion,
   cross-request lookup, restart/fallback, and prefetch cancellation. T033's
   harness/case implementation boundary is now closed; the previous
   source-sealed repetitions are retained as historical evidence. The
   canonical transport repair after that seal reopens T020/T022; a new
   source-bound host/CPU run is required.

The 2026-08-26 source-seal-v5 G0--G2 manifests (I01--I20, 38/38) remain useful
development evidence, but the code correction above changed the source subject.
They are historical rather than the final T020 seal. M11--M14 have host/CPU
real-Provider checkpoints, but the 42/42 matrix was also sealed before the
canonical transport repair and must be rerun. The earlier M09 native signal
exit did not reproduce in that pre-repair three-process M09 set and remains
preserved as historical negative evidence.

## Audit inventory

| Item | Result |
|---|---|
| User stories | 5 |
| Functional requirements | 82 identifiers, including FR-030a, FR-032a, FR-079, and FR-080 |
| Success criteria | 21 |
| Tasks | 43 total; 34 complete, 9 open (T043 plus T020/T022 and downstream qualification/closure) |
| Requirement traceability | 82/82 mapped |
| Structural audit | PASS |
| Spec Kit prerequisites | PASS |
| Whitespace/patch integrity | PASS |
| New TLV source census | No non-Spec use of `0xF685..0xF698` found; current G0 contract/traceability manifest is `PASS` |

### Historical verification snapshots

The detailed runs below are retained for diagnosis and chronology. They are
not current acceptance evidence because their source identities differ from
the repaired subject; the fresh convergence verdict above does not retroactively
promote them.

The following are retained development and historical qualification snapshots;
they describe the subjects that were run before the current convergence audit
and do not close any currently open task:

- Earlier `./build/unit-tests` checkpoints: 600 cases, no errors detected; the focused
  `Spec175*,ConversationState*` selection also passes all 33 cases.
- An earlier focused streaming/conversation/provider regression:
  112 passed (including the campaign Request-ID and receipt-scope regressions).
  The broader Spec175/168/170 sweep also passes 349 tests with 10 expected skips and one
  warning; it remains regression evidence, not a formal G0--G2 qualification
  manifest.
- The historical source-sealed Spec175 Python gate passed 197/197 registered
  targets, and the historical native integration gate passed 20/20 cases; a
  three-repeat healthy-case run recorded 38/38 results with no failures.
  Those manifests predate the current source corrections and therefore cannot
  serve as the current T020 seal. A later subject passes G0 with zero blockers,
  G1 with 219 passed and zero failed/skipped, and G2 with 38/38 registered
  results across I01--I20, but that subject was not used for the 42/42 G3 run.
- An earlier real MiniNDN launcher checkpoint removed only explicitly named, unowned stale
  `/run/nfd/<node>.sock` entries after `nfd-stop`; active listeners are a hard
  error. The stale/active safety regression passes (35 cases in the focused
  launcher gate).
- An earlier `tests/python/test_spec175_conversation.py` checkpoint: 36 passed (including failed
  prefetch recovery, stale-flight cancellation, and Provider-boot cleanup).
- An earlier strict Spec Kit structural snapshot: PASS (73 requirements, 34
  tasks, 27 accepted tasks); it is superseded by the frozen 82-FR/21-SC/42-task
  contract and the current 26-closed/16-open ledger above.
- Four fresh root MiniNDN probes against the corrected source now pass on the
  tiny four-Provider fixture: M01 healthy, M02 reordered publication, M03
  duplicate publication, and M04 one-loss/republication. M04's first attempt
  exposed an unused stale NFD socket from an earlier run; the clean rerun
  passed after the launcher guard removed that exact unowned path. These are
  development probes only, not the required 42/42 G3 matrix.
- A post-guard root M01 smoke (`seed=1750008`) also passed with one final
  Response, eight tokens, 13 bounded event retries, and clean teardown. Its
  manifest is `/tmp/spec175-m01-socket-2l4uY3/spec175-case-result.json`.
- The current-source M11 conversation run (`seed=1750001`) passed through four
  real Provider processes and emitted eight matched handler spans across all
  four roles. The launcher now binds the metadata-only
  `ndnsf-di-spec175-provider-timing-v1` report into the case result and rejects
  missing roles, unmatched spans, malformed timing, or negative durations. See
  `evidence/t015-provider-timing-20260827.md`; this is one execution checkpoint,
  not repeated T015/T020 qualification.
- The post-fix real M01 run used `/spec175-M01-1750001` as the actual NDNSF
  Request ID, streamed eight ordered events, returned one terminal response,
  and closed all four Provider spans. Earlier M01--M07 results that only used
  the campaign ID as a label are diagnostic and cannot qualify request-lineage
  claims. See `evidence/implementation-closure-request-id-20260827.md`.
- The previous T022 subject ran all M01--M14 cases three times from one fresh
  output root under its then-current T020 source seal. The strict G3 validator
  accepted 42/42 entries, with
  no missing case, nonzero process, topology mismatch, admission-control drift,
  or missing conversation evidence. See
  `results/spec175/g3/spec175-g3-current-20260827.json`; its immutable SHA-256
  is recorded in `evidence/t020-t022-current-gates-20260827.md`. Later source
  fixes make this historical regression evidence rather than current closure.
- After the receipt-scope correction, an intermediate source was resealed and the
  native G2 runner passed all 20 registered I01--I20 cases once with no missing
  case. The immutable source-seal, contract-manifest, and G2-manifest hashes
  are recorded together in
  `evidence/current-g2-one-repeat-integrity-20260827.md`; that intermediate
  checkpoint was later superseded by the final T020/T022 closure.
- The final repository-readiness subject passed G0, native unit, Python 222/222,
  G2 38/38, and G3 42/42 under source-seal SHA-256
  `194b9742442c1eba9353f8c96faf5b50dc846c83e146a3eddbad7b0409e15a93`.
  The G3 manifest is
  `results/spec175/g3/spec175-g3-post-repo-readiness-r5-20260827.json`; see
  `evidence/t022-post-repo-readiness-g3-20260828.md`.

The repository-wide Python suite remains diagnostic: 2,078 passed, 54 failed,
and 18 skipped in the current run. The failures are in historical Spec127--173
and stale host/toolchain/UAV fixtures; none is used to close or reopen a
Spec175 task without reproducing through a Spec175 owner. This does not waive
the open implementation or qualification gates below.

## Intent and architecture audit

### Intent fidelity: PASS

The design now covers every point requested in the discussion:

- ordinary unary/YOLO-style calls remain one-way and unchanged;
- one LLM turn performs prefill followed by the automatic Stage0 -> StageN ->
  token-feedback -> Stage0 decode loop;
- each Provider owns one complete role and only that role's model state;
- the application may continue several independent conversations without
  resending old context on a healthy path;
- the User keeps protected logical transcript/token history, while Providers
  keep model state; neither side is incorrectly treated as owning both;
- inactive conversation state may move from GPU to bounded host RAM, while model
  weights remain resident;
- all source changes and fast tests precede one final SIF/Tiger candidate.

### Necessity and scope: PASS

The new mechanisms are necessary for the requested behavior. A stable
`conversationId` alone cannot prove cache compatibility; one role's hit cannot
prove that every selected role has the same prefix; and a raw KV handle would
leak model/runtime details into the application API. The aggregate checkpoint,
per-role receipts, protected transcript record, exact readiness barrier, and
linear context epoch address those specific gaps.

The scope remains bounded. The feature explicitly excludes cross-Provider state
migration, disk/NVMe spill, shared-prefix reuse between conversations,
conversation branching/merging, speculative decoding, and continuous batching.

### Ownership and dependency direction: PASS

| Layer | Accepted ownership |
|---|---|
| Generic NDNSF Core | Opaque stream and versioned control transport only |
| APPClient/AutomaticPlanningCoordinator | Protected transcript, parent validation, fresh turn authority, role-set transaction, application futures |
| Provider runtime/state manager | Exact role-local tensors, residency, pin/prefetch/eviction, receipts |
| Qwen adapter | Chat-template/tokenizer prefix proof and complete hybrid state schema |
| Experiment/packaging layer | Registered cases, manifests, final SIF/Tiger launch only |

No application-facing Provider list, cache pointer, state tensor, or local path
is introduced. Full-context fallback may select a new role map, but it is
explicitly labelled full prefill and never reported as state reuse.

### State-machine consistency: PASS

The audit found and corrected two important ambiguities during this revision:

1. successful conversation completion now promotes request-scoped state into
   the conversation manager before releasing it; the old ordinary terminal
   cleanup rule no longer destroys the state needed by the next turn; and
2. storage tier (`GPU_RESIDENT`, `HOST_RESIDENT`, `EVICTED`) is separate from
   lifecycle (`IDLE`, `PREFETCHING`, `PINNED`, `COMMITTING`), so a pinned entry
   still has an unambiguous physical location.

The adapter must prove the exact prefix-extension equation before delta prefill.
If the chat template/tokenizer changes or the protected transcript is missing,
the runtime cannot claim reuse.

### Wire and compatibility: CONDITIONAL PASS

The generic `StreamCompletionV1` wire remains unchanged. Conversation metadata
uses separate additive NDNSF-DI blocks and exact signed/encrypted role receipt
Data. This avoids silently changing the decoder rules of an already versioned
Core completion block. The proposed TLV range is currently unused, but T029 and
T032 must add it to the executable collision gate and bind the final census to
the source seal.

### Security and privacy: CONDITIONAL PASS

The design binds checkpoints and receipts to requester, service, security
domain, model, plan/role map, Provider/boot/cache epoch, prefix, expiry, and
existing trust-schema identities. `conversationId` is explicitly not authority.
Prompt/messages, token history, checkpoints, receipts, secrets, and state tensor
contents are excluded from logs and public evidence. User transcript state is
stored through the existing envelope-key-protected RuntimeJournal; Provider
state remains within the trusted Provider process and is released/zeroized on
eviction under its runtime policy.

This remains conditional until mutation tests prove wrong requester/service,
forged/expired checkpoint, wrong journal key, incomplete role set, replayed
parent, and Provider restart are rejected before any incompatible runner call.

## Current-code reality

CodeGraph confirms that `APPClient.request_streaming(...)` accepts an optional
`ConversationContinuation` while preserving the existing full-context path
when omitted. `ConversationCoordinator` owns fresh per-turn Request and
generation identities, protected transcript/checkpoint CAS, exact delta-prefix
validation, and explicit fallback. `ProviderConversationStateManager` and the
native `ConversationStateStore` keep request-local state separate from
conversation-scoped state and implement promotion, bounded host/GPU residency,
single-flight prefetch, cancellation, expiry, and Provider-boot invalidation.
The host launcher exposes M11--M14 and the G2 runner records I16--I20.

The planning path now seals a `ConversationTurnBindingV1` after ACK-driven
placement and places the same successor-epoch/service/role-map/request
commitment in every V3 Selection. A resumed turn additionally projects one
compact role-local `ConversationStateReferenceV1` per Provider. The native
handler validates both objects against the actual assignment and resolves the
reference in the Provider-owned store before setting
`NativeEpochCoordinatorConfig::conversationStateBinding`. The epoch coordinator
also returns the exact finalized role state rather than leaving promotion to an
ambiguous cache search. The handler now publishes a compact, signed/encrypted
state-ready record, stages that state, publishes the Provider receipt, and
waits for the exact User control before committing or rolling back the
candidate. Committed entries bind the aggregate checkpoint digest, and the
Python owner validates one readiness record from every selected role before
admitting a resumed delta prefill. This closes the local
Selection-to-state/readiness ordering seam and implements bounded terminal-prefix
`CHECKPOINT_FINALIZE` plus Provider commit-ack validation, but not durable
all-role acknowledgement evidence or the real multi-process acceptance
boundary. The current T031 implementation boundary is closed; durable all-role
evidence is owned by T034/G6C and the real multi-process acceptance boundary
by T022/G3. This paragraph is retained as a historical checkpoint.

The ONNX source now validates a one-to-one `*_out -> *_in` state mapping and
uses the successor input name for the retained CUDA allocation. Python feed
validation requires every graph-declared input on decode. The native adapter
now materializes adapter-certified `attention_mask`, `position_ids`, and
`cache_position` tensors only from authenticated generation lineage, and the
formal Qwen reference oracle now performs one full-prompt prefill followed by
one-token decode while carrying every declared attention-KV,
recurrent-attention, and convolution state family. The streamed adapter no
longer copies complete state to host at every token and exports it once at the
terminal boundary; the exact Qwen3.6 CUDA graph and the unary multi-role
transaction's zero-host-round-trip behavior remain unqualified. T013's
implementation boundary is closed; T025 owns the eventual real 27B/CUDA
residency proof.

Conversation-state eviction, Provider-boot invalidation, and explicit store
clear now wipe retained tensor-bundle payload bytes before releasing each
entry. This closes the local release/zeroization seam; it does not establish
the still-open cross-process Provider ownership or CUDA residency evidence.

The Provider collaboration receiver now also rejects reserved
`user-control-v1` records whose name targets a different Provider, while
continuing to admit ordinary Provider-to-Provider records addressed to the
original User. This closes cross-Provider control fan-out at the local
receiver boundary.

Existing request-scoped hits must not be relabelled as multi-turn completion
evidence. The source-seal-v5 G0--G2 manifests also do not replace real MiniNDN
or exact-SIF evidence and must be regenerated by T020 after the implementation
queue closes.

## Findings and required closure

| ID | Severity | Finding | Required owner |
|---|---|---|---|
| A175-01 | HISTORICAL (host/CPU replay) | The prior same-seal 42/42 matrix exercised the four-Provider Ready/receipt/commit paths, fresh Requests, exact later-request lookup, delta prefill, and atomic conversation epochs. The repaired source is now running a new 42-process T022 matrix; exact-SIF/CUDA replay remains separately gated. | T022/T023/T025 |
| A175-02 | HIGH | CUDA `*_out -> *_in` mapping and adapter-owned causal position materialization are implemented, and the streamed adapter suppresses per-token complete-state host copies. The explicit request generation identity now reaches the V3 contract and readiness record; terminal/error cleanup now removes the session-keyed device state. The exact Qwen3.6 CUDA graph and the unary multi-role transaction's zero-host-round-trip proof remain unqualified. | T025 |
| A175-03 | HISTORICAL (pre-transport-repair G3 PASS) | The `20260901-v3-boundary-fix` source seal passed T020 G0--G2 and the strict host/CPU manifest recorded all 42 M01--M14 entries PASS. The canonical transport repair changed the production publication/Selection path, so this evidence is retained but no longer authorizes a candidate. | [`results/spec175/g3/qualification-manifest-20260901-v3-boundary-fix.json`](../../results/spec175/g3/qualification-manifest-20260901-v3-boundary-fix.json), [`evidence/t022-g3-v3-boundary-fix-20260901.md`](evidence/t022-g3-v3-boundary-fix-20260901.md) |
| A175-04 | SUPERSEDED BY C175-07 | No exact candidate had previously included the corrected source. A v5 exact SIF now exists and its preflights pass, but G4 fails at the first canonical object fetch; no G5/G6/G6C/G7 evidence exists for it. | T023, T025--T027, and T034 |
| A175-05 | HISTORICAL (pre-transport-repair G3 PASS) | The production Python User drove the real four-Provider native path through the model/task-first streamed API, and the pre-repair same-seal matrix recorded all 42 M01--M14 processes PASS. The repaired source requires a new source-bound matrix before exact-SIF/CUDA qualification. | request-ID regression; historical T022 manifest |
| A175-06 | MEDIUM | Appended-input tokenization must be proved prefix-stable for the exact Qwen tokenizer/chat template; a delta assembled only from message text is insufficient. The generic/tiny implementation boundary is closed, but the exact Qwen qualification remains open. | T025/T034 |
| A175-07 | MEDIUM | Actual GPU-host-GPU state movement cannot be established by CPU tier emulation. | T034/G6C |
| A175-08 | CLOSED | The flaky `ConversationStateStoreSeparatesCrossRequestState` test observed transient PREFETCHING state after waiting for completion. It now asserts the final single-transfer state and passed 100 repetitions. | native unit regression |
| A175-09 | LOW | T029--T034 IDs were appended after older evidence references, so visual task-number order differs from execution order. | Follow the dependency graph; do not execute file order |
| A175-10 | HISTORICAL (pre-transport-repair G3 PASS) | Production conversation checkpoints use purpose- and identity-separated journal-derived authentication keys; focused checks and the pre-repair M11--M14 matrix validated the binding. A new source-bound matrix is required after the canonical transport repair. | Python security regression; historical T022 G3 manifest |
| A175-11 | HISTORICAL (pre-transport-repair G3 PASS) | M11--M14 formal mode fails closed on test-only emulation; the pre-repair strict matrix validated receipt, promotion, commit-ack, lookup, isolation, restart, and cancellation across 12 real Provider processes. | historical T022 G3 manifest |
| A175-12 | HISTORICAL (pre-transport-repair G3 PASS) | The production coordinator previews/signs the exact checkpoint, publishes role-bound COMMIT controls before persistence, and rolls back on failure; the pre-repair matrix included M11--M14 network execution. | local transaction regression; historical T022 G3 manifest |
| C175-07 | CLOSED AT IMPLEMENTATION BOUNDARY; QUALIFICATION RESET REQUIRED | Exact-SIF G4 exposed that V3 selected a canonical `/ndnsf-di/...` identity while the tiny/Qwen publisher supplied a different transport name. The repaired coordinator, tiny ensurer, and Qwen registration path now bind canonical identity to encrypted fetch transport, and focused tests cover both mappings. The prior v5 candidate remains invalid because its source seal predates this repair. | T020: create a new source seal and G0--G3 subject; T023: add the canonical-root/source post-Selection probe and replay G4 |
| A175-13 | CLOSED (local boundary) | The native staged-promotion commits now reject a non-empty binding checkpoint on the legacy no-checkpoint overload, and the authenticated two-argument overload rejects a non-empty binding checkpoint that differs from the aggregate checkpoint supplied by the COMMIT control. This closes caller-mismatch paths without changing the staged binding shape used by the current control protocol. | `ConversationStateStoreResolvesOnlyExactSelectionCommitment`; native unit regression |
| A175-14 | CLOSED (local boundary) | Both V2 and V3 `AutomaticInferenceHandle` conversation metadata now carry the exact sealed `plan_digest` consumed by the asynchronous promotion path. A completed streamed turn can therefore pass the plan-binding check instead of failing after the final response with a missing metadata field. | Spec175 Python/API regression (174 passed) |
| A175-15 | CLOSED (host/CPU boundary) | The Python Provider-state bridge restores failed prefetches to a retryable state, fences stale cancelled workers, and releases aliased state views during Provider-boot invalidation. Native Provider binding clears stale staged-promotion side-index entries and rejects mismatched or unauthenticated binding digests. Repeated M11--M14 closes real Provider-process ownership on CPU; CUDA residency remains A175-02/T034. | conversation regression; T022 G3 manifest |
| A175-16 | CLOSED (local regression) | The four-Provider production-ingress test incorrectly used asynchronous ACK vector position as Provider identity and then dereferenced a missing role-map iterator after a failed assertion. Selection role construction is now Provider-name keyed and the negative assertion is non-fatal. The candidate-only V3 role-spec helper remains source-compatible, and literal `"cpu"` device identities are rejected while CPU remains represented by an empty device tuple. | `evidence/current-local-regression-20260827.md`; focused Python 203 passed; native unit 600 and integration suites pass |
| A175-17 | CLOSED (local boundary) | ConversationStateStore now wipes every retained TensorBundle payload before eviction, expiry cleanup, Provider-boot invalidation, or explicit clear. This satisfies the local state-release/zeroization requirement without claiming cross-process or CUDA qualification. | native unit `*ConversationState*` (6 cases) |
| A175-18 | CLOSED (local boundary) | Reserved User COMMIT/ROLLBACK collaboration controls are now recipient-filtered by the Provider name encoded in the collaboration Data name; ordinary Provider-to-Provider data keeps the original User requester convention. | `ServiceProvider::onCollaborationDataMessage`; full integration suite |
| A175-19 | CLOSED (local execution boundary) | A lineage-bearing coordinator epoch could previously enter a runner's complete `runStreamed()` loop, duplicating token events and advancing state more than once per epoch. The worker now selects one-shot `run()` for coordinator-owned epochs, and standalone ONNX `runStreamed()` rejects authenticated lineage rather than reusing stale prefix positions. | `ProviderRoleWorker`; `OnnxRuntimeModelRunner`; native unit 600; integration suite |
| A175-20 | CLOSED (local evidence boundary) | Conversation continuation restored a promoted parent state but reported zero `prefixWorkAvoided` at request epoch zero. The coordinator now records the exact promoted parent prefix length while keeping conversation and request-local hit metrics distinct; the three-token parent regression reports three avoided tokens. | `NativeEpochCoordinator`; `NativeEpochCoordinatorRestoresConversationStateAndExtendsPrefix`; native unit 600 |
| A175-21 | CLOSED (launcher hygiene; execution still privilege-gated) | `nfd-stop` can leave a per-node Unix socket path behind. The Spec175 launcher now probes only the exact topology-owned paths, removes a path only after an unowned connection refusal, and rejects an active listener. When an unprivileged launcher cannot unlink a root-owned stale path it records an explicit deferred cleanup instead of claiming removal; a root MiniNDN start remains required for execution. | `cleanup_unused_nfd_sockets`; focused real-MiniNDN launcher regression (35 passed) |
| A175-22 | CLOSED (host/CPU observation) | `AutomaticStreamingHandle` records monotonic event/terminal/error timestamps and exposes a metadata-only `timing_summary`; the pre-repair T022 subject repeated the real native-Provider timing evidence across M01--M14. Performance qualification remains T027, and the host matrix must be regenerated after the canonical transport repair. | timing-summary regression; historical T022/T027 |
| A175-23 | CLOSED (local fail-closed boundary) | Direct `ConversationCoordinator.commit_turn()` callers now receive the same origin request/generation/service/requester/security-domain checks as `prepare_checkpoint()`, so a mismatched receipt cannot bypass the normal lineage contract. `AutomaticStreamingHandle` also contains user error-callback exceptions so they cannot escape the delivery thread or replace the original terminal error. | `conversation.py`, `app_sdk/placement.py`; correction regression 71 passed |
| A175-24 | CLOSED (local authorization-scope boundary) | `prepare_checkpoint()` and `commit_turn()` now require every receipt to share one non-empty service, requester identity, security-domain digest, and per-role model digest. A coordinator with blank constructor scope can no longer let the first receipt implicitly authorize a mixed receipt set. | `conversation.py`; `test_checkpoint_rejects_mixed_receipt_scope_when_coordinator_scope_is_blank` (3 parameterized cases) |

## Frozen execution order

```text
T035/T036 design-code and candidate-closure implementation [complete]
  -> T024 proven Tiger profile + semantic delta validator + sealed submit [closed]
  -> T020 new source seal and G0-G2 [open after T024]
  -> T022 same-seal G3 replay (M01-M14, 42/42) [open after T020]
  -> T023 one final SIF and exact-SIF 42-case replay
  -> T025 G5
  -> T026 G6
  -> T034 G6C
  -> T027 G7
  -> T028 closure
```

Historical note: the pre-profile canonical-transport source passed G0--G3 and is recorded in
`evidence/t020-t022-canonical-transport-20260901.md`, but it cannot authorize a
new SIF because T024's production submit boundary did not implement the proven
profile comparator it claimed. The subsequent SIF passed both preflights and
M01, and its full G4 run was stopped at 24/42 when this finding was confirmed.
All of that evidence is diagnostic. Upload, Slurm allocation, and Tiger remain
deferred until one new T020 -> T022 -> T023 chain passes without changing the
validated launch profile.

## Final audit decision

**Superseding decision (2026-09-01): RUNTIME AND TIGER-SUBMISSION
IMPLEMENTATION PASS; QUALIFICATION BLOCKED.** The ordinary-V3 boundary, post-Selection canonical assembly,
native terminal sampling/text, opaque coordinator state, adapter-owned
conversation transfer seam, and filterable diagnostics are closed at their
focused implementation boundaries. C175-07 is closed at the implementation
boundary by the checked-in proven Tiger profile, semantic field comparator,
gate-specific allowlist, sealed explicit environment, and byte-identical
validated/submitted command proof. The unresolved gap is fresh T020/T022
qualification after this source correction.

Exactly 34/42 tasks are closed. T024 is closed at the implementation boundary.
Its tracked submit/checklist correction invalidates the pre-profile T020/T022
seal; therefore T020 and T022 follow it once, not repeatedly after each diagnostic.
Only then may T023 build one final SIF and run one complete exact-SIF replay.
No old candidate, partial G4 root, direct `.sbatch`, ad-hoc wrapper, or inherited
environment can authorize Tiger.
