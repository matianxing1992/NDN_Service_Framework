# Detailed Requirements Registry: NDNSF-DI Streamed Invocation

> This file preserves the complete 82 requirements (FR-001..FR-080 plus
> FR-030a and FR-032a) and SC-001..SC-021
> definitions that existed before the deployment-focused Spec rewrite. Only
> the numbered requirement and success-criterion definitions are normative.
> Historical checkpoints and execution guidance in this file are retained for
> auditability and are not an execution entry point. Use `../spec.md`,
> `../plan.md`, and `../tasks.md` to run the current qualification.

**Feature Branch**: `Experimental`

**Created**: 2026-08-21

**Status**: **r5 IS UNQUALIFIED; A CORRECTED CANDIDATE IS REQUIRED.** The
feature contract contains 82 functional requirements and 21 success criteria.
The current release target is one corrected candidate-bound deployment, not a
matrix or retry campaign. Conversation residency and performance, when
registered by the workload, are observations inside that deployment rather than
separate campaigns.

**Execution simplification (2026-09-02).** The numbered FR/SC definitions
remain the detailed product contract, but the executable route is now
`T020 -> T022 -> T025 -> T028`. Any historical G3/G4/G4T/G5/G6/G6C/G7 or
T026/T034/T027 label is an internal acceptance check or archived evidence under
this one route. All checks share one source seal, SIF, profile, workload, model
identity, and result lineage; they do not authorize additional Tiger jobs or
parameter sweeps.

**Execution-control correction (2026-09-02).** The exact-SIF driver now runs one
registered case in a fresh process group/output directory, enforces a bounded
watchdog, records four-Provider readiness, reaps the process group on timeout,
and emits explicit terminal evidence. An interrupted run is an incomplete
subject, never a partial qualification. The r5 SIF/replay are diagnostic only;
the corrected candidate must establish a new local-release subject.

**Current reseal checkpoint (2026-09-02).** Earlier native, NDN-SVS, and
repository-readiness repairs are retained in the evidence archive. They are
not additional execution instructions. Candidate r5 passed its previous local
release gate, then its `multi-provider` deployment (Job `208200`) failed before
ACK/Selection because the exact-SIF runtime rejected the planning manifest
candidate digest. See
[`evidence/t025-tiger-deployment-208200.md`](../evidence/t025-tiger-deployment-208200.md)
and [`evidence/t028-final-closure-20260902.md`](../evidence/t028-final-closure-20260902.md).
The bundle-builder repair must pass T020/T022 before one corrected candidate is
sealed. No matrix, diagnostic wrapper, or parameter campaign is authorized.

**Validation gate:** design-to-code convergence is a mandatory precondition to
formal testing. A test run is not acceptance evidence merely because it exits
successfully: the current spec/contracts, effective configuration, production
call path, security and cleanup behavior, terminal ownership, logging, and
evidence schema must first be reconciled against the source. Any behavior-
affecting repair invalidates the prior audit and formal manifests. Runtime and
release-boundary implementation convergence are `PASS`; broad qualification is
not authorized until the corrected source receives one new exact
source/configuration identity satisfying the per-run sign-off card.

### Current completion boundary

The r5 local release and Tiger result are superseded for qualification because
the planner/bundle source contract changed. T020 and T022 must be rerun for the
corrected candidate; then T025 is one deployment and T028 is one closure. The
r5 evidence remains diagnostic and cannot be combined with the corrected
candidate.

Historical matrix rows, superseded SIFs, and diagnostic jobs remain useful for
provenance only. They cannot close the corrected candidate.

The 82-FR/21-SC contract is frozen for this Spec. A newly requested feature,
optimization, topology, model mode, or experiment belongs in a follow-up Spec
unless it corrects a contradiction in this frozen contract. Such a correction
must name the affected task and earliest invalidated gate; it must not silently
append a new completion condition to an already implemented task.

### TigerCluster deployment objective

The objective is to run one immutable NDNSF-DI candidate through the core
user-visible distributed-inference path on TigerCluster. The `multi-provider`
gate is the only Tiger invocation: its readiness, stage loading, ACK-derived
placement, streamed prefill/decode, terminal Response, and cleanup are sections
of one deployment record. Generic unary compatibility and focused security
checks remain local evidence and are not duplicated as Tiger jobs. Performance
and conversation observations, if naturally emitted, are recorded in that same
record without changing the subject.

### Canonical experiment contract (mandatory)

The detailed field-ownership and retry contract is maintained in
[`contracts/tiger-experiment-profile-v1.md`](contracts/tiger-experiment-profile-v1.md)
and is the shared reference for this section, quickstart, the repository
submitter, and the iTiger skill.

Every Spec175 experiment is one immutable subject, not a hand-edited job
script. The subject identity is the tuple
`(sourceSeal, profileDigest, runner/configuration-tree digest, SIF digest,
gate, runId, model/artifact identities)`. A result is valid only when all
evidence files, the rendered command, and the terminal oracle bind to the same
tuple. A job number, a copied script, or an old successful log is not a
substitute for that identity.

All Tiger gates use the same checked-in launcher and configuration tree:

```text
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  <gate> <proven-profile.json> <run-record.json>
```

For the local G2 gate, the native subject is always
`build/integration-tests`; the runner probes its Boost.Test suite/tree before
starting any I-case. `build/unit-tests`, a renamed copy, or a binary whose
registered case inventory does not match the manifest is rejected before the
case loop and produces no misleading per-case failures.

The run record is data, not a second launcher. It may change only fields in the
profile's gate-specific allowlist (normally candidate/run/model or artifact
identity; stage-readiness may additionally select the consumed device
mapping). Provider count, GPU count, seed, Slurm resources, working directory,
helper paths, launch order, identities, routes, and timeout/deadline values are
fixed by the checked-in profile/job for that gate. If one of these must change,
create a new profile/job and source seal; never override it through the shell
environment. An exported variable is not a registered parameter unless a
checked-in production consumer reads it and the validator proves its effect.

Before any upload, staging, model transfer, SSH mutation, or scheduler
allocation, the repository validator must render the complete command and
effective environment, compare every field with the proven profile, run the
negative mutation suite, and prove zero external side effects on rejection.
The exact validated bytes are the bytes submitted. Direct `sbatch`, a second
wrapper, `--export=ALL`, ambient configuration, and post-validation command
reconstruction are forbidden.

The local route is linear and bounded: G0 freezes the candidate, G-L runs the
focused owner checks plus one host/CPU smoke and one exact-SIF smoke, and G-T
uses `submit.sh multi-provider` once on TigerCluster. A broad unit target is
supporting evidence and is rerun only when a changed owner invalidates it; no
generated repetition manifest, matrix, or hand-edited shell loop is part of
Spec175. Any behavior, dependency, helper, configuration, or evidence-schema
change invalidates the seal and restarts at the earliest affected gate.
Failures are first classified as source/runtime, packaging/ABI, or
cluster-substrate failures and reduced to the cheapest gate that can reproduce
them; changing unrelated parameters to make a later job pass is not a repair.

**Required pre-test order:** freeze the design and effective configuration;
inspect the real production entry points and dependencies; record and repair
every design/code discrepancy; run the smallest focused regression for each
repair; re-audit the repaired production path; require a fresh `PASS`; only
then run complete tests, qualification, benchmarks, or experiments. A broad
test started before that `PASS` is diagnostic history and cannot close a task
or support a result claim.

### No-repeat rule for Tiger failures

The operator-facing explanation of the historical drift and its correction is
recorded in [`evidence/t024-one-launcher-root-cause-20260902.md`](evidence/t024-one-launcher-root-cause-20260902.md).
It is part of this Spec's control-plane evidence: a prior successful job is
usable only when its complete subject tuple, not merely its script name or job
ID, is identical to the current tuple.

The repeated historical failures were process failures, not evidence that a
new Tiger allocation was needed: an ambient variable was exported but not
consumed, a relative artifact was resolved from the wrong `cwd`, a host-built
extension crossed the SIF ABI boundary, a stale SIF/result was reused after a
source change, or the wrong executable was presented as the integration gate.
Consequently, a new job ID never repairs the subject. Every canonical launch
must first write its rendered effective configuration, command digest, and
source/profile/SIF/model identity tuple; the validator must compare these
before any remote side effect. A failed run is retained with its layer
classification and reproduced at the cheapest owning gate. Retrying the same
tuple with a new seed, wrapper, environment, binary, or output directory is
forbidden; those changes create a new subject and require a new seal and
lower-gate qualification.

The normal operator workflow is therefore intentionally short:

```text
checked-in profile + checked-in launcher + allowlisted run record
    -> render/hash effective command and environment
    -> mutation/side-effect preflight
    -> submit the exact rendered bytes
```

The registered parameter table in
[`contracts/tiger-experiment-profile-v1.md`](contracts/tiger-experiment-profile-v1.md)
is normative. It prevents a “small” timeout, seed, `cwd`, provider-count, or
resource edit from silently becoming a different subject. If the experiment
needs one of those changes, the correct action is a new profile and new source
seal, not another hand-edited invocation.

### One launcher, no active variation axes

Spec175 has one checked-in launcher invocation: `submit.sh multi-provider`.
Provider/GPU count, resources, topology, routes, identities, timeouts, working
directory, workload, model/runtime, and seed are fixed by the profile. The run
record may carry only candidate, model, output, and other profile-allowlisted
identity fields. Unknown or changed values must be rejected before SSH, upload,
staging, or `sbatch`; a changed subject requires a new profile and a follow-up
Spec, not another row here. For the current qualification, the Tiger launcher
is submitted once; a scheduler, node, or filesystem failure is preserved as
negative evidence rather than retried within Spec175.

**Input**: Extend the verified NDNSF-DI design so one selected multi-Provider
ONNX plan can produce a local-LLM-like ordered token stream and continue an
explicit multi-turn conversation from exact Provider-local state, while
preserving one terminal Response per turn, unary compatibility, request-scoped
placement, security, and one bounded local-release-to-Tiger deployment route.

### State-scope boundary: one Request versus one Conversation

Spec175 defines two different kinds of model state. They MUST be named and
measured separately; the word “KV-cache hit” without one of these qualifiers is
not valid evidence.

The second scope is a real **cross-Request, same-Conversation KV-cache**. It is
not merely a saved transcript, a checkpoint lookup, or a request-local entry
re-keyed with a new request ID. “Cross-Request” means that the later turn has a
fresh NDNSF Request, attempt, plan, and generation identity; “same-Conversation”
means that it presents the authenticated successor of the exact committed
`conversationId`/`contextEpoch` lineage. Only the Provider-owned promoted state
bundle may supply the prior prefix to that later Request.

| Scope | What it supports | Identity and lifetime | What a later Request may do |
|---|---|---|---|
| **Request-local decode state** | The prefill-to-decode loop for one invocation | `requestId`, attempt, plan, generation, role, Provider, complete model/state identity, and consumed-token prefix; created by that Request's prefill and released at terminal cleanup | Nothing. A fresh Request MUST NOT search or reuse this entry, even when prompt bytes or `conversationId` match. |
| **Conversation-scoped state** | Continuing a completed turn without recomputing its committed prefix | Stable `conversationId` plus linear `contextEpoch`, exact transcript/prefix, model/adapter/layout, one-to-one role map, Provider boot/cache epoch, and security scope; created only by an all-role promotion transaction after successful turn completion | An explicit `APPEND_DELTA` Request MAY restore this entry after validating its opaque checkpoint and exact prefix extension; it then creates fresh Request/generation authority. |

The handoff is explicit: `request-local state -> finalized turn candidate ->
all-role checkpoint commit -> conversation-scoped state`. A normal non-conversation
Request never performs this promotion. Conversation state remains Provider-local;
the application receives only an authenticated opaque checkpoint, never tensors,
device pointers, cache addresses, or a Provider list. If the conversation entry
is missing, expired, evicted, incompatible, or unavailable, the coordinator
must use the explicitly authorized full-context fallback or fail; it must not
silently treat a request-local hit as conversation reuse.

Evidence MUST report request-local prefill/hit/commit/cleanup separately from
conversation promotion/hit/prefetch/eviction/fallback, including separate state
bytes and transfer latency. This separation is required for both CPU semantic
tests and CUDA residency tests.

The intended two-turn lifecycle is therefore:

```text
Turn 1 / Request R1 (FULL_CONTEXT)
  full prompt prefill -> request-local state for R1
  -> decode epochs for R1
  -> terminal prefix finalization
  -> all-role promotion and checkpoint C@epoch=1

Turn 2 / fresh Request R2 (APPEND_DELTA)
  new message only + opaque C@epoch=1
  -> validate transcript/prefix/placement/security
  -> restore each Provider's conversation-scoped state
  -> delta prefill over the appended canonical suffix
  -> request-local state for R2 and decode epochs for R2
  -> terminal prefix finalization
  -> all-role promotion and checkpoint C@epoch=2
```

Thus, the first turn's request-local cache is not itself the cross-request
cache. It becomes eligible for reuse only through the explicit finalize,
all-role commit, and promotion transaction. A second request that repeats the
full transcript is a valid correctness fallback, but it is not evidence of
conversation-cache reuse. A request-local hit and a conversation-scoped hit
must use different metric names and different acceptance assertions.

For avoidance of doubt, Spec175 uses the following evidence rule:

```text
same Request + later decode epoch
  = request-local KV-cache hit

fresh Request + same authenticated conversation successor
  + promoted Provider state restored
  + appended-suffix-only prefill
  = cross-Request same-Conversation KV-cache hit

fresh Request + repeated full transcript
  = full-context recomputation (never a conversation-cache hit)
```

The second case is the feature's cross-request cache objective. It MUST be
measured per Provider role, including whether the state was GPU-resident or
host-resident and prefetched, and MUST be rejected if any role lacks an exact
compatible promoted entry. The checkpoint authorizes and identifies the
continuation; it never carries or substitutes for the KV/recurrent/convolution
state itself.

### Historical evidence boundary and execution priority

The following paragraphs preserve the 2026-08-31 checkpoint for provenance;
they are superseded by the current T042 PASS and must not be read as the
active task frontier. The current report is
[`evidence/design-code-audit-20260901-canonical-transport.md`](evidence/design-code-audit-20260901-canonical-transport.md).

**Historical 2026-08-31 audit correction:**
source inspection found that several existing tests prove isolated components
while the production path bypasses or weakens their contracts. The next work is
therefore the finite reopened T031 implementation task, followed by T042, not
another G0/SIF/Tiger cycle. T035--T041 are already closed at their focused
boundaries. The exact findings, source locations, required tests, and evidence
boundary are frozen in
[`evidence/design-code-audit-20260831.md`](evidence/design-code-audit-20260831.md).
The remainder of this section is retained as historical checkpoint context and
MUST NOT be read as current promotion authority.

The earlier implementation queue and one historical G0--G5 chain produced
valuable evidence, but later Provider/teardown/repository-readiness repairs
changed the active subject. The current exact-SIF terminal manifest is 38/42,
so neither the older replay nor its Tiger control/G5 results can unlock G6.
At the 2026-08-30 checkpoint, the authorized next work was T035--T036: finish machine-enforced candidate
closure and the live repository/terminal lifecycle contracts, then create one
fresh G0--G4 candidate before returning to T025/T026.

- **Historical completed checkpoints**: before the 2026-08-31 source audit,
  T001--T022, T024, and T029--T033 satisfied their then-named implementation or
  prior-candidate gates (28 of 36). T031 was reopened at that checkpoint, T037
  was closed, and reopened T031 and T042 were the remaining convergence tasks; T020/T022 formal
  gates were also reopened at that checkpoint, so the historical count was 32
  of 42 closed. T023,
  T025--T028, and T034 remain open after the production-path convergence
  sequence.
  T021 closes the builder/preflight implementation, not a promotable candidate.
  No source change is allowed after the G3 subject is frozen without returning
  to G0.
- **Core streamed path**: production C++ Request/ACK/Selection/event/End/Response
  transport, Normal and Targeted entry points, exact-Interest recovery, bounded
  Provider/callback queues, final-role-only event-key projection, and the low-
  level Python callback/native-writer bindings exist. Focused lifecycle and
  stream integration suites pass. The contract-level Python
  `StreamedInvocation` handle, async iterator, awaitable result/cancel,
  status/metrics, decoder/options surface, and callback-versus-iterator
  single-consumer guard are implemented and pass a real host NFD/Controller/
  Python-Provider/Python-User regression. These remain host-development
  results. The prior G1/G2 manifests pass against their historical source seal,
  but the updated cache contract reopens T020; they are not current promotion
  or SIF/Tiger evidence. An earlier
  CPython-3.10 candidate-SIF proof passed for local r30, but r30 is diagnostic
  only. The generic Python boundary
  is recorded separately in

  `evidence/t009-python-native-stream-20260823.md`; it must not be confused
  with the automatic DI process proof in T015.

- **Current automatic-stream checkpoint (2026-08-27)**: a current-source M01
  run now drives the Python model/task-first `request_streaming` API through
  four independent native Providers and V3 automatic planning.  It delivers
  eight ordered events and one terminal response with four matched Provider
  timing spans.  The run exposed and fixed a labeling defect in which this
  event-producing path marked the context/response as `FULL`; both now carry
  `TOKEN_STREAMING`.  This proves the host/tiny-ONNX process seam only and does
  not close the frozen Qwen workload, repeated G2, CUDA, SIF, or Tiger gates.

- **Conversation-state implementation checkpoint (2026-08-26)**: the Python
  Provider owner now has a two-phase `ConversationStatePromotionTransaction`.
  It keeps every role's request-local state until all receipts, quotas, and the
  protected checkpoint/journal boundary validate, then exposes all
  conversation entries together. Parent conflicts, duplicate successor keys,
  quota failures, and journal errors leave no partial promotion; a later
  request still cannot search the request-local store. The streamed M11--M14
  probe uses this transaction rather than independently promoting roles. The
  current focused Spec175/Provider-generation Python regression passes 204
  cases; the broader Spec175/168/170 regression passes 349 cases with 10
  expected skips and remains regression evidence rather than a formal
  qualification gate;
  the dedicated conversation regression passes 36 cases. At that historical
  checkpoint T030/T031 were reported as closing the implementation boundary;
  the 2026-08-31 code audit later reopened T031 because its GPU/host
  transitions were accounting-only. CUDA residency and post-G3 qualification
  remain open under T031/T022/T025.
- **Provider receipt/control checkpoint (2026-08-26)**: the native Provider now
  emits a Provider-authored conversation receipt only after a finalized role
  state is staged, and retains that candidate as `COMMITTING` until the User
  publishes an authenticated, exact COMMIT or ROLLBACK control on the shared
  request-scoped collaboration topic. The User-side streaming handle validates
  the complete receipt set, commits the protected checkpoint, and publishes one
  role-bound control per selected Provider before exposing the terminal result.
  Focused native/Python tests cover receipt digest parity, staged invisibility,
  expiry cleanup, and the completion-before-attachment race. Real host/CPU
  MiniNDN checkpoints now cover M11--M14 with four independent Providers,
  including restart/fallback and Provider-side prefetch-cancellation controls.
  They close neither the current same-seal G3 matrix nor CUDA residency; the
  M11--M14 implementation itself is closed by T030--T033 and its repeated host
  qualification is owned by T022.
- **Incremental generation**: the tiny ONNX fixture, exact decode-state identity,
  persistent CPU ORT sessions, one-plan loop, exact activation/feedback names,
  and one-/two-/four-role Python oracle pass. Native NDNSF-DI I01-I03/I15 now
  pass three fresh processes each through Request/ACK/plan/Selection, per-epoch
  authenticated activation/feedback, ORT, ordered events, and one End/Response.
  The gate also proves clean termination of every selected Provider and an
  ACK-driven role-map permutation. `AutomaticPlanningCoordinator` now seals
  TOKEN_FEEDBACK, operation stride/capabilities, generation limits/EOS/sampling,
  and adapter-owned attention-KV/recurrent/convolution state I/O. The deployed
  native Provider validates that authenticated contract and drives the same
  coordinator; the production Python user requests stateful token streaming,
  while the legacy Python Provider rejects an incomplete stateful path.
  The Python streaming handle now records monotonic callback timestamps and
  exposes metadata-only `timing_summary` values for TTFT, every inter-token
  interval, terminal latency, and attempt/replacement counts. This removes the
  earlier timing-observation gap without fabricating Provider lifecycle spans;
  complete per-role TTFT/ITL/component evidence and the automatic production
  process qualification remain T015/T027 work. Controlled replacement, the
  live I13 transport boundary, and the automatic native-harness path now have
  native evidence; this is not yet a production Python workload-process
  qualification.
  Those earlier sealed results proved epoch and adapter semantics but used
  manual state feedback. The current-source formal I01-I03/I15 paths now use
  the production coordinator/runtime for Provider-owned predecessor lookup,
  pinning, candidate commit/rollback, terminal cleanup, missing-state failure,
  and state-free NDN publication. I01 records 8 commits, 7 hits, 0 misses, and
  one cleanup for the exact eight-token oracle; I15 also passes with a genuine
  four-Provider role-map permutation. `NativeProviderHandler` now derives the
  complete static identity template from the authenticated V3 assembly,
  projection, artifact, runner, Provider, boot, and runtime-ABI authorities.
  `GenerationEpochLineageV1` carries one authenticated logical token-prefix
  digest/count and position commitment across activation and TOKEN_FEEDBACK
  edges; the coordinator assigns explicit state and predecessor inference
  epochs while preserving one cache-incarnation epoch. The focused 14-case
  I01-I13/I15 matrix passes, including on-link tamper rejection before any
  application callback. The registered same-artifact CPU control now also
  compares this cached production path with a no-predecessor full-prefix
  reference. Both produce the exact token sequence `4,5,6,7,8,9,10,2`; the
  cached runner consumes one new token at every epoch while representing prefix
  lengths 1--8, whereas the control consumes full-prefix lengths 1--8. The
  machine-derived avoided prefix work is 0--7 tokens per epoch, 28 in total.
  This closes CPU cache-effectiveness semantics together with the completed
  mutation, concurrency, attempt/restart, cancellation/deadline,
  publication-failure, and capacity matrix. It makes no timing, CUDA-residency,
  or Qwen3.6-27B claim. T014/T015 and the replacement source-bound G2 seal are
  closed; exact Qwen3.6 CUDA residency remains owned by T025.
- **Coordinator/runner boundary correction (2026-08-27)**: an authenticated
  generation lineage is owned by `NativeEpochCoordinator`, which invokes one
  unary Provider-role transition per inference epoch. `ProviderRoleWorker` no
  longer starts a complete `runStreamed()` loop for each coordinator epoch,
  and the ONNX adapter rejects direct `runStreamed()` calls carrying that
  lineage. Standalone streamed calls without coordinator lineage remain
  compatible. The explicit `PREFILL`, `DECODE`, and bounded
  `CHECKPOINT_FINALIZE` transition kinds are validated before execution, so a
  finalization lineage cannot accidentally enter ordinary decode. Focused
  native regressions and the full unit suite cover this boundary. Production
  Python automatic multi-role execution is closed by T014/T015; real
  Qwen3.6/CUDA qualification remains open under T025.
- **Historical gate boundary (superseded 2026-09-01)**: revision `e4d67cb8` passed G0 with zero blockers, native
  unit tests, the 222-pass Python gate, all 38 registered G2 processes, and the
  42/42 G3 M01--M14 matrix under one source seal. This closes T020 and T022 for
  the corrected automatic Provider-state and repository-startup subject. The
  full repository Python run remains diagnostic because it contains historical
  Spec127--Spec173 frozen-source drift. The historical M09 signal exit and old
  G3/SIF subjects remain explicit regression evidence. G5--G7 remain unpassed.
- **Historical formal G3 closure (2026-08-28; superseded 2026-09-01)**: the fresh root
  `results/spec175/g3/post-repo-readiness-r5-20260827` contains exactly three
  independent PASS processes for every M01--M14 case. The strict manifest
  validator accepted all 42 results with the frozen topology, four Providers,
  admission disabled, tiny-ONNX runtime, workload seed `1750001`, complete
  runner logs, and the M11--M14 conversation evidence. The promotion manifest
  is `results/spec175/g3/spec175-g3-post-repo-readiness-r5-20260827.json`.
- **Historical exact-SIF G4 closure (2026-08-28; superseded 2026-09-01)**: candidate
  `spec175-final-candidate-replay42c` uses SIF
  `sha256:63539a1adffa4d8500c56d34104d81971aa72a29958723cd35143bd52b98fbd1`
  and passed the host-substrate and in-SIF runtime preflights. The original
  replay had 41/42 PASS because `M14-r1` hit a transient pre-Selection startup
  race; its directory and log remain retained. An independent same-input
  `M14-r4` passed, and the repaired aggregate records 42 PASS entries while
  explicitly naming that replacement. This closes T023/G4 locally, but does
  not claim a clean first pass, retry-free execution, Tiger execution, or
  stateful Qwen3.6-27B CUDA readiness. See
  `evidence/t023-g4-exact-sif-replay-replay42c-20260828.md`.
- **Provider-readiness correction (2026-08-30)**: the fresh host replay
  `results/spec175/g3/current-20260830i` stopped at M14-r1 because the
  pipeline reached the first request with only three Providers. Its merged log
  appeared to place `LLM_PIPELINE_PROVIDER_READY` before native registration,
  but Python stdout and ndn-cxx logs use different streams, so that line order
  is not causal evidence. Source inspection confirms the native
  `ServiceProvider::init()` call is synchronous inside `ServiceProvider.start()`.
  The fix keeps this explicit start barrier before the readiness marker. A
  bounded M14 rerun at
  `results/spec175/g3/diagnostic-M14-provider-start-20260830k` and a fresh M04
  rerun at `results/spec175/g3/diagnostic-M04-provider-start-20260830m` then
  passed with four Providers and a final Response. These are diagnostic
  confirmations only: the source changed after the prior G0--G4 identities,
  so a fresh source seal, G0--G3 matrix, SIF, and exact-SIF replay remain
  mandatory. See
  `evidence/t022b-provider-readiness-20260830.md`.
- **Host MiniNDN checkpoint (2026-08-25)**: the selected current-source matrix
  contains 30 PASS processes across M01--M10 with the frozen four-Provider
  configuration. An additional M09 process reached the registered
  `EventTimeout` after three events and then exited with signal 11 before a case
  result was written. A same-subject isolated rerun passed. The crash remains
  an unclassified native/lifetime failure and reopens G3; it cannot be removed
  by selecting only successful repetitions. The cursor-1, setup-only, M07
  preparation, and M09 signal-exit runs remain explicit negative evidence and
  are not pooled into a promotion manifest. See
  `evidence/t022-g3-current-20260825n.md`.
- **SIF boundary**: the strengthened local preflight executes inside the
  candidate SIF and checks CPython-3.10 SOABI/import, native `ldd` closure,
  CUDAExecutionProvider, and absence of PyTorch/Transformers residue. The
  candidate `spec175-final-candidate-20260825l` was built once from the then
  sealed source and 30/30 G3 host manifest. Its SIF digest is
  `sha256:250d510ae13b1d8567b0d83935df70fa14e7bf9dc46fbecbecbee40548262cd8`;
  its runtime and host-substrate preflights pass only for their respective
  layers. The exact replay failed 30/30 at NFD readiness, and a follow-up
  launcher smoke reached certificate bootstrap but failed decrypt. Candidate l
  is therefore **invalidated and diagnostic-only**; the patched host runner
  requires a new G0-G3 seal and a new SIF before G4.
  G4 keeps MiniNDN, Mininet, Open vSwitch, NLSR, the topology, and the replay
  driver on the host. Inside each MiniNDN namespace the host must use the exact
  Apptainer binary and candidate SIF to start the SIF-contained NFD and
  NDNSF/ONNX Runtime application processes. MiniNDN and its routing substrate
  are not SIF payloads, and the SIF does not need a self-contained MiniNDN
  driver or fixture. The host harness, topology, tiny ONNX fixture, and SIF are
  separate hash-bound G4 inputs. Superseded candidates remain diagnostic only.
  See `evidence/t021-local-sif-current-20260825l.md` and the eventual G4
  closure record.
- **Implementation-first execution policy (2026-08-26)**: while any required
  implementation task is open, run only the focused compile/unit/integration
  checks owned by the behavior being changed. Do not repeatedly reseal formal
  G0/G1/G2 manifests or rerun the full G3 matrix after every patch. Once all
  implementation and pre-frozen launcher/harness work closes, run one
  current-source G0--G3 sequence. Freeze that passing subject, build exactly
  one local SIF, run G4, and only then submit Tiger. Any later source,
  contract, workload, or dependency change invalidates the candidate and
  returns the workflow to implementation plus G0; it must not start a rebuild
  loop.
- **Historical Tiger success boundary**: NDNSF-DI has previously completed real
  TigerCluster executions. Spec170 D0 Job 189483 ran one controller, one User,
  four Providers, real NFD, isolated PIB/TPM state, four ACKs, Selection, CPU
  ONNX execution, and a final Response. Current-r23 D2b Job 201039 completed a
  two-Provider cross-node CUDA path, and D2h Jobs 201045/201046 completed both
  declared heterogeneous mappings. These runs prove that the local-SIF ->
  hash-verified Tiger execution route is viable; they do not qualify the
  changed Spec175 source, streamed protocol, stateful Qwen3.6-27B artifacts, or
  performance target. Spec175 MUST treat those jobs as an operator/configuration
  control through one checked-in proven-profile capsule, not as prose memory:
  preserve Apptainer 1.5.3 parity, explicit bundle `cwd`, isolated HOME/PIB per
  identity, exact SIF staging, child exit checks, and no MiniNDN or SIF build on
  Tiger. The fully rendered command/configuration MUST match that profile except
  for the gate's explicit allowlist. After G4, one bounded current-SIF Tiger
  control using the proven D0-shaped lifecycle MUST pass before the 27B G5/G6
  jobs are attempted.
- **Diagnostic-only 27B evidence**: job 202864 showed that the pinned three
  Qwen3.6-27B ONNX stages could execute as a CUDA full-context chain. Its
  campaign used `useCache=false` and recorded an empty decoded transcript, so
  it is neither the accepted stateful token oracle nor G5/G6 evidence. Older
  Spec162 jobs, source overlays, and full-context-per-token loops remain
  deployment diagnostics only.
- **Model-tier boundary**: the committed tiny ONNX fixture is the deterministic
  CPU protocol/state oracle. A Qwen3-0.6B CPU (or optional CUDA) run is only a
  low-cost adapter/control-flow diagnostic. It cannot close the pinned
  Qwen3.6-27B CUDA subject, because it does not exercise the same model state,
  memory pressure, artifact split, or GPU execution. The 27B full-context chain
  is also insufficient until T025 proves complete stateful ONNX prefill and
  incremental decode.

- **Historical native multi-Provider boundary checkpoint**: the request-scoped epoch coordinator in
  `contracts/native-epoch-coordinator-v1.md` repeats each selected role per
  epoch over authenticated DATA_V1 activation/feedback edges and preserves one
  final Event/End/Response owner. Its bounded terminal marker drains upstream
  roles without an extra ORT transition. The shared automatic V3 placement and
  deployed Provider entry point now construct and validate that request-scoped
  contract. The current focused I01-I13/I15 matrix uses authenticated V3
  projections, Provider-owned state, common logical-prefix lineage, and
  explicit predecessor epochs; it no longer uses harness-managed decode-state
  feedback. The pre-correction G2 manifest remains historical. At that earlier
  checkpoint T014/T015 stayed open for the frozen workload; the current
  source-bound native oracle now closes those implementation tasks. Focused
  native harness evidence still does not substitute for the later T020/T022
  qualification subject.

Therefore this document is an implementation contract, not a report that the
feature or its 20 token/s target has already been achieved.

## User Scenarios & Testing *(mandatory)*

### T035--T036 implementation checkpoint (2026-08-30)

The repository now contains the cheap, deterministic prevention layer required
before another qualification cycle. `packaging/ndnsf-di-container/bin/
spec175-candidate-closure` validates the seven candidate-tuple identities,
ownership-plane transition, G0--G4 terminal manifests, and the complete
transitive executable/configuration closure without network or scheduler side
effects. `submit.sh` invokes it before model staging, checklist processing,
command discovery, SSH, upload, remote mutation, or `sbatch`; focused mutation
tests cover every registered closure kind, SIF-byte drift, gate mismatch, and
multi-plane transitions.

`Experiments/spec175_repo_bootstrap.py publish` now performs a bounded,
non-mutating CAPABILITY request through the same initialized publisher
`ServiceUser` over `/NDNSF/DistributedRepo/Object/v1/STATUS`. The parent
MiniNDN runner requires both its validated
`ndnsf-di-spec175-repo-route-probe-v1` PASS record and marker before releasing
the publication barrier. Case results now include
`ndnsf-di-spec175-terminal-evidence-v1`; all owned children are reaped before
the result is written, and G3/exact-SIF validators reject missing or surviving
children. Focused tests pass, and the T035/T036 implementation boundaries are
closed. The previous M13/G0--G4 records remain historical because the current
source changed again in the canonical transport repair; a fresh source-bound
sequence must reuse these guards before promotion.

Provider readiness follows the same fail-closed rule: the public
`LLM_PIPELINE_PROVIDER_READY` marker is emitted only after native handlers and
filters are synchronously registered and the native event loop has started.
A marker emitted earlier is process-start evidence, not service readiness. The
marker and registration logs may use different output streams; merged
stdout/stderr line order is therefore not an ordering proof. Qualification
must use the `ServiceProvider.start()` call-return barrier or separate
machine-readable readiness records. The repository route probe may retry only
within its bounded budget, with a fresh
request ID for each attempt and all attempts retained in its JSON report; a
retry never replaces a failed matrix repetition.

### User Story 1 - Consume One Ordered Streamed Invocation (Priority: P1)

An application submits one logical invocation, receives zero or more typed progress
events while the request is executing, and then receives exactly one complete
terminal result. An LLM application sees incremental token text before the full
answer is complete; a unary application continues to use the existing one-result
API without any behavior change.

**Why this priority**: Without a stable streamed-invocation contract, the
distributed LLM can only appear as many unrelated requests or one delayed final
response, neither of which behaves like a normal interactive LLM.

**Independent Test**: A deterministic in-process provider emits a known event
sequence followed by one result. The client observes the events in cursor order,
delivers each cursor once, obtains the complete final result, and the unchanged
unary test suite still passes.

**Acceptance Scenarios**:

1. **Given** an authorized streamed request and a healthy provider, **When** the
   provider emits five events and one final result, **Then** the application
   receives cursors 1 through 5 exactly once before the final result.
2. **Given** a request that legitimately completes without intermediate events,
   **When** the provider publishes the final result, **Then** the client returns
   that result without waiting for an event that will never exist.
3. **Given** an existing unary service, **When** it is invoked through the
   existing unary API, **Then** it still produces exactly one final response and
   exposes no streamed lifecycle.
4. **Given** an active streamed invocation, **When** the application cancels it,
   **Then** no later event or final result is delivered to the application.
5. **Given** a normal streamed service invocation, **When** the application names
   only the unified service and request, **Then** NDNSF discovers willing
   Providers through the normal Request/ACK path without requiring a caller-
   supplied Provider list.

---

### User Story 2 - Generate Tokens Through One Selected ONNX Plan (Priority: P1)

An LLM application performs one request/ACK/placement/Selection cycle, runs
prefill once, and then repeatedly decodes through the same selected pipeline.
The final role returns each sampled token both to the first role for the next
decode epoch and to the user as an ordered application event. Provider-local
decode state is reused only when its exact identity remains compatible. That
state includes full-attention KV tensors and every model-required recurrent or
convolution state used by hybrid attention layers.

**Why this priority**: Repeating discovery, placement, model preparation, and
prefill for every token would dominate latency and would not be true streaming
generation.

**Independent Test**: A CPU-only small canonical ONNX fixture with fixed token
inputs and seed runs one prefill plus multiple decode epochs across a fixed
one-to-one role/Provider plan. The emitted token IDs and final transcript match
the local oracle, and evidence proves that placement and prefill occurred once.

**Acceptance Scenarios**:

1. **Given** a selected multi-stage plan, **When** generation produces N>=1 tokens,
   **Then** the streamed handle, ACK closure, committed plan, Selection set,
   one prefill transition, N-1 decode transitions, N ordered token events, and complete terminal
   Response all retain the same original Request and request identifier; no
   second service Request is created after planning.
2. **Given** a valid provider-local decode-state entry for the exact accepted prefix,
   **When** the next token epoch starts, **Then** the provider reuses that entry
   and processes only the incremental input required by its role.
3. **Given** any mismatch in model, adapter, role split, prefix, position,
   precision/layout, runtime ABI, security domain, provider boot epoch, attempt,
   or generation identity, **When** reuse is considered, **Then** the entry is
   rejected and clean computation or a new plan is used.
4. **Given** EOS, an accepted stop sequence, maximum generated-token count,
   deadline, or cancellation, **When** that condition occurs, **Then** generation
   terminates with the corresponding reason without requiring the maximum token
   count to be reached for EOS or stop completion.
5. **Given** the registered prompt, artifact, split, seed, and output oracle,
   **When** the cached incremental path is compared with a full-prefix reference
   path, **Then** both produce identical tokens while the cached path performs
   prefill once, consumes only one newly admitted token/current activation per
   later epoch, and reports the exact prefix work avoided. A cache-entry lookup
   or hit counter without this execution evidence is not accepted as an
   effective KV-cache.

---

### User Story 3 - Recover Without Mixing Attempts or Duplicating Output (Priority: P2)

A user receives a correct stream despite Data duplication, bounded loss,
reordering, delayed packets, cancellation, or one bounded replan. Old-attempt
events and responses cannot enter the accepted transcript, and the framework
does not guess that execution is safe to repeat when that would violate
application semantics.

**Why this priority**: Streaming exposes partial progress over time. Recovery
must preserve both NDN delivery semantics and the verified-delivery attempt,
plan, authorization, and final-result invariants established by Spec 174.

**Independent Test**: Deterministic fault injection duplicates, reorders, drops,
delays, replays, and tampers with events; cancels an invocation; and replaces a
provider once. The user either reconstructs the exact oracle transcript under
the accepted attempt or terminates with a specific error, never a mixed or
silently incomplete result.

**Acceptance Scenarios**:

1. **Given** duplicate and reordered event Data, **When** all required cursors
   become available before the deadline, **Then** callbacks occur once in cursor
   order and the final transcript digest matches.
2. **Given** a missing cursor within the retained window, **When** its retry
   budget remains, **Then** the user re-expresses the exact Interest and resumes
   ordered delivery after the gap is filled.
3. **Given** a stale or replayed event from another attempt, plan, generation, or
   stream epoch, **When** it arrives, **Then** it is rejected before application
   delivery.
4. **Given** a final-role failure and no exactly compatible decode-state checkpoint,
   **When** bounded replacement is enabled for the application request, **Then**
   a new attempt recomputes from the prompt plus the committed accepted-token
   prefix; it does not silently migrate incompatible decode state.
5. **Given** a non-idempotent application that has not opted into replacement,
   **When** the selected provider fails after execution may have begun, **Then**
   the request terminates rather than being re-executed automatically.

---

### User Story 4 - Qualify Before Spending TigerCluster Resources (Priority: P3)

A developer can prove the complete streamed path with unit tests, CPU-only
integration tests, and a representative host/CPU MiniNDN topology before
building one final immutable SIF. The SIF is replayed locally as a packaging
check, then promoted to TigerCluster. Tiger then verifies real ONNX Runtime CUDA
execution and measures interactive generation without using a different code or
artifact path.

**Why this priority**: Previous distributed-inference work lost time when ABI,
bundle, path, or protocol defects reached Tiger before local gates exercised the
same path.

**Independent Test**: Each gate consumes the same versioned contract and emits
a manifest. Tiger submission is refused unless all lower gates pass and the
candidate SIF passes the native runtime preflight.

**Cost-ordered execution rule**: During implementation, run the smallest
focused compile/unit/integration check that proves the behavior under change;
formal G0--G3 qualification is deferred until all implementation tasks and
their focused checks close. SIF construction, SIF replay, remote upload, Slurm
allocation, and Tiger execution are release-candidate steps, not debugging
steps. They begin only after the one frozen current-source G0--G3 sequence
reports a complete `PASS`; after that point the source/workload identity is
frozen and exactly one final SIF is built. Any later source, contract,
workload, or dependency change invalidates the candidate and returns the
workflow to implementation plus G0 rather than starting another SIF/Tiger
attempt.

**Acceptance Scenarios**:

1. **Given** any failed unit, integration, host MiniNDN, security-negative, SIF
   preflight, or final-SIF replay gate, **When** promotion is requested, **Then**
   SIF promotion and Tiger submission are refused with the failed gate named.
2. **Given** all local gates pass, **When** the exact SIF hash is promoted,
   **Then** Tiger verifies that hash, runs the same workload contract, proves
   CUDA ONNX Runtime model execution with no CPU compute fallback (bounded CPU
   shape/control nodes are allowed by ORT), and emits correctness and
   latency evidence.

3. **Given** G0, G1, G2, or G3 is missing or failed, **When** a packaging or
   Tiger command is requested, **Then** the command is refused before SIF
   creation, upload, allocation, or model staging, and the named failed gate
   is returned. A pre-existing diagnostic SIF cannot satisfy this condition.
4. **Given** a functional run whose throughput is below the performance target,
   **When** results are summarized, **Then** it is reported as functional but not
   performance-qualified; no 20-token/s claim is made.
5. **Given** G4 passes for a new candidate, **When** Tiger promotion begins,
   **Then** a bounded current-SIF D0-shaped control first reproduces the proven
   Controller -> four Provider -> User Request/ACK/Selection/Response lifecycle
   with isolated identities, explicit bundle `cwd`, exact staged SIF hash, and
   all child exits checked. The 27B stage or multi-Provider campaign is refused
   if this control fails.
6. **Given** a source, host-replay, submit-bundle, workload, model, identity, or
   resource-envelope input changes, **When** promotion is requested, **Then**
   the candidate record names the changed plane, invalidates the affected gates,
   and refuses to combine the changed input with stale PASS evidence.
7. **Given** the repository has emitted local registration or permission-ready
   markers, **When** a publisher is about to mutate the repository, **Then** a
   bounded, non-mutating request from the same identity must first traverse the
   exact service route and receive a validated ACK; a marker or fixed delay is
   not accepted as network readiness.
8. **Given** a missing helper, environment mismatch, invalid working directory,
   incomplete identity set, shared PIB, insufficient resource request, or stale
   digest, **When** the exact Tiger command is prepared, **Then** a process-only
   closure check fails before SSH, upload, remote mutation, or Slurm allocation.

---

### User Story 5 - Continue a Conversation Without Recomputing Its Full Prefix (Priority: P2)

An application can start a conversation with a full prompt and later submit a
new turn containing only appended input plus an opaque checkpoint returned by
the preceding successful turn. NDNSF-DI verifies that every selected role has
the exact compatible Provider-local state, restores paused state from host RAM
when necessary, prefills only the appended tokens, and then resumes the normal
automatic decode loop. Application code never transports KV tensors, cache
addresses, or a Provider list. This is not ordinary request-local decode-state
reuse: request-local state exists only to advance tokens inside one Request,
whereas a conversation checkpoint deliberately retains the completed turn's
exact model prefix after that Request ends so that a later, fresh Request can
continue it.

**Why this priority**: A local-LLM-like API is incomplete if every turn must
recompute the entire conversation prefix. Cross-turn reuse must nevertheless
remain explicit because stale, partial, or wrongly authorized state would make
the result incorrect or unsafe.

**Independent Test**: Run at least three independent conversations with two
turns each. For every second turn, compare the token IDs and final text with a
full-transcript oracle, prove that only the appended prefix is processed, and
exercise GPU-resident, host-resident, unavailable, mismatched, expired, and
concurrent-parent cases without sending model state over NDN.

**Acceptance Scenarios**:

1. **Given** a first turn in `FULL_CONTEXT` mode, **When** it completes, **Then**
   the application receives one opaque conversation checkpoint only after all
   selected roles have committed compatible state for the same prefix and
   context epoch.
2. **Given** that checkpoint and appended user input, **When** the next turn uses
   `APPEND_DELTA`, **Then** it receives a new request and generation identity,
   reuses the same exact Provider-role placement, restores each role's local
   state, processes only the appended tokens during turn prefill, and produces
   the same result as full-context recomputation.
3. **Given** a paused conversation whose role state is in host RAM, **When** it
   is scheduled again, **Then** state prefetch begins before execution, all role
   state reaches the execution tier before the turn starts, and no partial
   role set is used.
4. **Given** a missing, evicted, expired, wrong-Provider, wrong-boot, wrong-model,
   wrong-plan, wrong-layout, wrong-security-domain, or wrong-prefix state,
   **When** authenticated full context is supplied and fallback is allowed,
   **Then** NDNSF-DI performs one explicit full-context prefill and records the
   reason; otherwise it fails explicitly without executing from partial state.
5. **Given** two turns that name the same parent checkpoint concurrently,
   **When** both try to commit the next context epoch, **Then** exactly one may
   advance the linear conversation and the other fails with a conflict; silent
   branching or mixed state is forbidden.
6. **Given** several conversations sharing the same selected Providers,
   **When** memory pressure moves inactive state between GPU and host RAM,
   **Then** state, metrics, eviction, and authorization remain isolated by
   conversation and role, while model weights remain resident on the GPU.
7. **Given** a successful turn, **When** it was not conversation-enabled,
   **Then** its request-local state is released at terminal completion and a
   later Request cannot reuse it. **Given** a conversation-enabled turn,
   **When** its last accepted output token or turn-finalizing template suffix is
   not yet represented by the live decode state, **Then** the adapter performs
   bounded state-only checkpoint-finalization transitions before promotion,
   emits no extra application token, and exposes no successor checkpoint until
   every role represents the exact completed-turn prefix.

### Edge Cases

- A final Response arrives before one or more already-declared event cursors.
- An End event is duplicated, arrives after cancellation, or disagrees with the
  terminal Response's cursor count or transcript digest.
- The provider completes before emitting any event.
- An event is validly signed but belongs to a different request, attempt, plan,
  generation, provider boot epoch, or stream epoch.
- A cursor gap is outside the provider retention window.
- An event payload is larger than the configured event wire-size limit.
- The application callback throws, blocks, or consumes more slowly than tokens
  are produced.
- The final-stage output queue reaches capacity.
- EOS is generated before the requested maximum token count.
- A stop string crosses token boundaries and requires incremental decoding.
- Unicode text is split across tokenizer pieces.
- The selected provider restarts and loses its decode state while old signed Data
  remains retrievable.
- A provider-local decode-state entry is evicted, missing, or has the correct
  session/stage key but the wrong request, attempt, plan, generation, prefix,
  provider-boot, cache-epoch, or predecessor-inference-epoch identity.
- Two concurrent generations compete for bounded decode-state capacity while
  one role has an in-flight candidate state that must not evict or overwrite its
  previously committed state.
- A CUDA execution returns correct tokens while copying the complete decode
  state through host memory on every epoch; this is functionally incremental but
  is not an effective qualified CUDA cache.
- A runtime reports decode-state hits but invokes a full-prefix runner, rebuilds
  the predecessor bundle, or cannot account for the actual input-token count and
  avoided prefix work; this is state bookkeeping, not effective cache reuse.
- One pipeline role fails before publishing its activation, after publishing
  its activation, or after the final role samples a token.
- Cancellation races with an event publication or the terminal Response.
- A malicious peer replays a valid ciphertext under a different cursor name.
- A unary response payload happens to contain fields resembling a stream event.
- Two concurrent invocations use the same model and prompt prefix but different
  authority or attempt identities.
- The event-encryption epoch key cannot be obtained or rotated before expiry.
- An authorized but unselected ACK Provider observes the Request but must not
  receive or decrypt the final-role event-encryption key.
- A continuation checkpoint is well formed but stale, expired, forged, or bound
  to a different requester, service, security domain, model, plan, role map,
  Provider boot, cache epoch, prefix digest, or context epoch.
- One role's conversation state exists while another selected role's state was
  evicted or lost after a Provider restart.
- Host-RAM pressure evicts an inactive conversation while a different
  conversation is pinned or being prefetched.
- A continuation is cancelled while one or more roles are moving state from
  host RAM to GPU.
- `APPEND_DELTA` is requested without a valid parent checkpoint and without an
  authenticated full-context fallback payload.
- Two new turns race to advance one parent context epoch.

## Requirements *(mandatory)*

### Functional Requirements

#### Public Invocation Contract

- **FR-001**: The framework MUST retain the current unary invocation contract:
  one request yields exactly one complete terminal Response or one terminal
  failure, with no required stream object or event callback.
- **FR-002**: The framework MUST add one application-neutral streamed invocation
  contract in which one request yields zero or more ordered typed events followed
  by exactly one complete terminal Response or one terminal failure. Its primary
  Normal surface MUST accept a unified service name and request without a
  Provider list, preserving request-scoped discovery through the existing
  Request/ACK path. An explicit single target is valid only for Targeted mode;
  any legacy candidate filter is a compatibility input and MUST NOT become the
  NDNSF-DI default or evaluation path.
- **FR-003**: The streamed contract MUST expose the immutable request identifier,
  event consumption, terminal-result retrieval, cancellation, status, and
  bounded metrics through one invocation handle.
- **FR-004**: The C++ and Python user/provider surfaces MUST express the same
  lifecycle and defaults; the Python user surface MUST support asynchronous
  iteration without requiring application-managed NDN Interests, and a Python
  Provider MUST publish through the Core-owned writer rather than a Python-only
  stream state machine.
- **FR-005**: LLM token generation MUST be an application use of the generic
  streamed contract, not a framework-level LLM-only request API or LLM-only wire
  message.
- **FR-006**: Targeted and normal selection MAY both create a streamed
  invocation, but they MUST share one event/result lifecycle rather than create
  separate Targeted-stream state machines. Targeted mode MUST reuse the existing
  one-time token pool and Targeted bootstrap/refill behavior: a cached token uses
  the selection-free fast path; a cache miss makes that same streamed invocation
  follow `TargetedBootstrapRequest`; an already in-flight refill permits the
  current bounded one-Provider normal path. None of these paths may create a new
  public API or silently downgrade the streamed request to unary. A multi-role
  NDNSF-DI streamed invocation MUST extend the existing deferred collaboration
  created by `BeginCollaboration`: its stream state, ACK-closed snapshot,
  `CommitCollaborationPlan`, Selection projections, event delivery, and terminal
  Response MUST share one Request identifier and one terminal owner within one
  attempt. Planning MUST NOT launch a second `RequestServiceStreaming` request
  or create a parallel discovery/selection state machine within that attempt.
  The sole exception is the explicit, application-authorized recovery attempt
  in FR-033..FR-037; it is a new internal Request with fresh one-time tokens,
  ACK closure, plan, Selection, event key, and stream epoch, while the public
  handle remains one logical invocation.
- **FR-007**: A streamed invocation MUST have exactly one terminal owner and MUST
  reject duplicate terminal claims.
- **FR-008**: Application callback failures MUST be contained, recorded, and
  converted to the configured cancellation/failure behavior without corrupting
  framework state.

#### Event Naming, Delivery, and Completion

- **FR-009**: Each event MUST be published as signed NDN Data under a
  deterministic producer-owned exact name derived from request, accepted
  attempt, plan, generation, stream epoch, and monotonically increasing cursor.
- **FR-010**: The framework MUST use exact-name Interests for event retrieval and
  MUST NOT require a separate predictive name-mapping publication for invocation
  events.
- **FR-011**: An event MUST bind its request identifier, attempt epoch, plan
  digest, generation identifier, stream epoch, cursor, event type, producer,
  payload digest, and terminal flag in authenticated content.
- **FR-012**: Cursor numbering MUST begin at 1 and increase by exactly 1 within
  one stream epoch; cursor 0 is reserved and invalid on the wire.
- **FR-013**: The consumer MUST pre-express only a bounded exact-Interest window,
  maintain an ordered reorder buffer, suppress duplicate application delivery,
  and re-express a missing cursor only within a bounded retry/deadline budget.
- **FR-014**: Network delivery MAY be at least once, but application delivery
  MUST be at most once per accepted `(attempt, streamEpoch, cursor)` and in
  strictly increasing cursor order.
- **FR-015**: The publisher MUST retain a bounded event window until terminal
  completion plus the declared grace period; an unrecoverable cursor outside the
  window MUST fail explicitly rather than skip silently.
- **FR-016**: An explicit authenticated End event MUST declare the final cursor,
  finish reason, generated-token count, transcript digest, and final-result
  digest reference. It MAY carry the last application event but MUST NOT replace
  the complete terminal Response.
- **FR-017**: The terminal Response MUST remain authoritative, contain the
  complete application result required by Spec 174, and bind the accepted End
  event identity and transcript digest.
- **FR-018**: A terminal Response received before all declared event cursors MUST
  be held until the gaps close or the invocation fails; it MUST NOT cause the
  application to observe an incomplete successful transcript.
- **FR-019**: FEC MUST be disabled by default for invocation events. Exact-name
  retry, caching, and bounded retention are the baseline recovery mechanisms.
- **FR-020**: One event's encoded wire size and one invocation's queued event
  count MUST have explicit limits; oversize or overflow MUST produce bounded
  backpressure or a declared failure and MUST NOT drop an accepted event.

#### Incremental Generation and Role Ownership

- **FR-021**: One LLM generation MUST use exactly one Request, one closed ACK
  snapshot, one accepted placement plan, and one Selection set per attempt; it
  MUST NOT repeat discovery or placement per generated token. The healthy path
  has exactly one attempt. An explicitly authorized replacement MAY create the
  one additional recovery Request defined by FR-033..FR-037, but it remains
  inside the same public logical invocation and stable generation identity.
- **FR-022**: The runtime MUST perform prompt prefill once per clean attempt and
  MUST perform subsequent decode epochs using only the new token at Stage 0 or
  the current upstream activation at a later stage, plus exact compatible
  provider-local decode state. For a hybrid model this state
  MUST include both full-attention KV tensors and the convolution/recurrent
  state required by linear-attention layers. Recomputing the complete prompt
  and accepted prefix for every healthy decode epoch is not a qualifying
  implementation of this requirement. The selected Provider runtime MUST own
  lookup, validation, candidate creation, atomic commit, eviction/fencing, and
  terminal cleanup of that state for its complete role. An application, test
  harness, dependency Data object, or caller MUST NOT manually copy a role's
  state output back into its next input to make the production path work. Every
  role transition MUST expose whether it was prefill, an exact-state hit, or an
  authorized recomputation, together with the actual new-input token/activation
  extent and the logical prefix extent already represented by local state; a hit
  MUST NOT be reported when the runner recomputes that prefix.
- **FR-023**: The final pipeline role MUST own sampling, incremental text
  decoding, End-event creation, and terminal-response assembly as part of its
  existing complete role; these functions MUST NOT create an additional
  placement role. The production native path MUST execute the sealed sampler
  and a standalone tokenizer, emit Unicode-safe nonempty `textDelta` values
  whenever the decoded oracle advances, and place the complete decoded text in
  the terminal Response. Token-ID-only output is diagnostic and cannot satisfy
  this requirement.
- **FR-024**: The final role MUST publish two logically separate outputs for a
  nonterminal token: an internal feedback object for the first role and an
  external application event for the user. The names, authorization audiences,
  retention, and acceptance checks of these objects MUST remain distinct.
- **FR-025**: Every ordinary pipeline role MUST have exactly one Provider and
  every selected Provider MUST own exactly one complete role. Spec 175 does not
  extend streamed generation to the existing Spec 174 TensorGroup/rank-role
  protocol; that independently specified protocol remains unchanged and outside
  the Spec 175 validation subject. Before Selection or role execution, the
  Spec175 entry point MUST reject a V2 placement result, a hybrid plan, tensor
  degree other than one, a TensorGroup/rank-role candidate, a missing role, or
  duplicate Provider ownership.
- **FR-026**: `PreSplitFirstStrategy` MUST remain the default placement strategy;
  alternative placement strategies MUST be explicit opt-ins and MUST preserve
  the same role and event contracts. Compatibility-only V2 strategies and
  `LayerReuseFirstStrategy` are not valid Spec175 defaults. The registered
  Qwen workload and all formal Spec175 launchers MUST name
  `PreSplitFirstStrategy` explicitly so configuration identity cannot drift.
- **FR-027**: Sampling parameters, tokenizer identity, model/adapter identity,
  chat-template digest, thinking mode, split identity, decode mode, modality
  closure, maximum tokens, EOS/stop rules, deterministic seed, and deadline MUST
  be sealed into the accepted request/plan identity. Spec175 qualification fixes
  `decodeMode=single-token-autoregressive`, `modality=text-only`, and
  `mtpEnabled=false`, and `thinkingMode=disabled`; neither MTP/speculative
  tokens, hidden thinking, nor a vision input may be enabled implicitly. The
  terminal Provider MUST execute the authenticated sampler parameters, not
  merely carry or compare their digest. V1 supports Greedy and sealed seeded
  Top-K/Top-P; an unsupported sampler MUST fail before generation.
- **FR-028**: EOS and stop completion MUST be valid before maximum-token count;
  maximum-token completion MUST be distinguishable from EOS, stop, deadline,
  cancellation, and failure.
- **FR-029**: Incremental text decoding MUST preserve tokenizer state across
  token boundaries and MUST correctly produce Unicode-safe text deltas and
  stop-sequence matching. Qualification MUST exercise the actual native
  terminal-role owner rather than an isolated Python helper, and the final
  decoded text, concatenated deltas, token sequence, and stop reason MUST agree
  with the standalone-tokenizer oracle.
- **FR-030**: The deployed runtime MUST use canonical ONNX artifacts, ONNX
  Runtime, and a standalone tokenizer boundary; PyTorch and Transformers MUST
  remain outside the deployed SIF and runtime dependency closure. A Spec175
  qualification Qwen artifact MUST be the language-model-only text subgraph:
  the source repository's vision encoder/projector and image/video preprocessing
  MUST be excluded from the exported stage graphs, cache, SIF dependency
  closure, and memory/performance claims. MTP/speculative heads MUST be absent
  or disabled; V1 samples exactly one next-token logit result per decode epoch.
  The artifact MUST use FP16 or FP32 tensor contracts; a BF16 graph MUST be
  rejected unless an explicit packed-BF16 binding is implemented, and MUST NOT
  be silently fed as FP32. Qualification artifacts MUST expose the
  adapter-certified prefill/decode inputs and outputs for every component of
  `DecodeStateBundleV1`; a full-context-only ONNX stage is reference/diagnostic
  evidence and cannot satisfy G5-G7. On the healthy CUDA path, the complete
  decode state MUST remain Provider-local and device-resident across epochs
  through persistent runtime bindings; only declared activation tensors,
  token-feedback control, and bounded scalar metadata may cross NDN or host
  control boundaries. A complete decode-state device-to-host-to-device round
  trip on every token is diagnostic-only and cannot satisfy G5-G7. This
  zero-round-trip requirement applies to the real distributed
  `NativeEpochCoordinator` path for every role; a direct single-runner adapter
  test cannot satisfy it. The Provider runtime MUST retain an opaque
  adapter-owned device-state handle across epochs and expose measured copy
  counters that distinguish model-state movement from activation/control I/O.
- **FR-030a**: The Qwen qualification manifest MUST distinguish the offline
  `pytorchReferenceTopToken` parity diagnostic from the deployment
  `referenceTopToken`. The latter MUST be generated by the canonical ONNX
  Runtime CUDA stage chain and MUST be the oracle used for streamed-token
  acceptance; a cross-runtime PyTorch/ORT token difference is reported, not
  silently substituted as the deployment oracle.

#### Decode-State Identity, Recovery, and Fencing

- **FR-031**: Decode-state reuse MUST require exact agreement on model and graph
  semantics, adapter, runner, role split/layers, logical model-token prefix
  digest and length, adapter-certified position state, precision/layout,
  runtime ABI, security domain, provider boot/cache epoch, request attempt,
  generation identity, state inference epoch, predecessor inference epoch, and
  the complete model-specific state schema (including attention-KV and
  recurrent/convolution components). The static fields MUST come only from the
  validated artifact, accepted plan projection, selected Provider, and loaded
  runtime; missing values fail before prefill and MUST NOT be replaced by
  generated placeholder digests. The dynamic prefix, position, state epoch,
  predecessor epoch MUST be derived by the request-scoped coordinator from the
  admitted prompt and authenticated token lineage. The Provider runtime owns a
  cache-incarnation epoch that remains constant throughout one entry lineage
  and changes only when that cache is reset or invalidated. These values
  MUST NOT be derived from role-local activation bytes, free-form runner
  metadata, session ID, or loop count alone. Every role in one pipeline epoch
  binds the same logical model-token prefix digest/count even though its local
  activation bytes differ. A role may consume only the state committed by its
  own immediately preceding successful epoch.
- **FR-032**: A decode-state mismatch, missing/evicted entry, provider restart,
  or failed candidate MUST never be treated as a partial match or silently
  zero-initialized during decode. It MUST produce a recorded miss/reject and
  either perform the explicitly permitted clean prefix recomputation or enter a
  new accepted plan. The previous committed state remains authoritative until
  an epoch's candidate output and downstream publication are accepted; active
  state is pinned against eviction, and cancellation, deadline, terminal
  completion, boot-ID change, or attempt fencing releases it within a bounded
  interval. Prefill creates the only valid state with no predecessor; every
  decode candidate explicitly names its state epoch and the immediately
  preceding committed state epoch. The model runner MUST NOT be invoked when
  that predecessor is absent or mismatched.
- **FR-032a**: Effective decode-state reuse MUST be established separately from
  store correctness. A fixed-input cached-versus-full-prefix control MUST prove
  exact output parity, one prefill followed by incremental inputs on the cached
  branch, no hidden full-prefix execution on a reported hit, and a positive,
  machine-derived amount of prefix work avoided. CPU qualification MUST prove
  these execution semantics and lifecycle counters without making a timing
  claim. CUDA qualification MUST additionally prove persistent device-resident
  state, zero complete-state host round trips after prefill, and lower median
  cached model-compute time in the registered alternating-order matched pairs.
  A store entry, identity match, hit counter, correct transcript, or faster
  end-to-end run by itself MUST NOT be described as an effective KV-cache.
- **FR-033**: Provider replacement MUST be bounded to at most one replacement per
  streamed invocation in this feature and MUST be disabled unless the application
  request declares that recomputation is permitted. Replacement is supported
  only for Normal deferred-collaboration mode in Spec 175; Targeted mode rejects
  replacement options rather than inventing a second token path.
- **FR-034**: Replacement MUST create attempt 2 as a new internal NDNSF Request
  with a distinct request ID, fresh one-time tokens, ACK closure, plan,
  Selection, event key, and stream epoch. The initial attempt is 1. Both attempts
  share the stable generation ID and public logical invocation. Data from the
  old attempt MAY remain retrievable but MUST be fenced from accepted
  application delivery and final-result assembly. Replacement MUST start only
  from a recoverable error whose validated stream/response binding identifies
  the failed Provider by absolute NDN name; the coordinator MUST NOT guess that
  identity from the terminal role or prior plan.
- **FR-035**: Without an exactly compatible protected decode-state checkpoint,
  replacement MUST seal the original prompt plus the committed accepted-token
  IDs, count, and digest into the encrypted recovery Request and accepted plan,
  then prefill that complete prefix exactly once. The recovery stream publishes
  only new continuation tokens; it MUST NOT replay already committed tokens as
  new application callbacks. Cross-Provider live decode-state migration is out
  of scope. The accepted V3 plan core and every selected Provider projection
  MUST also bind a digest of the exact encrypted recovery Request bytes; a
  Request/plan digest mismatch fails before model execution.
- **FR-036**: Cancellation, deadline, failed authorization, terminal error, and
  replacement MUST stop new decode work, fence callbacks and publication, release
  bounded queues, and zeroize protected plaintext/decode state as required by Spec 174.
- **FR-037**: A retry or replacement MUST NOT re-execute a non-idempotent
  application after execution may have begun unless the application explicitly
  opted into the corresponding recomputation contract.
- **FR-038**: Stale, duplicate, cross-attempt, cross-plan, cross-generation, and
  post-cancellation events and responses MUST be rejected before application
  callbacks.

#### Security and Resource Control

- **FR-039**: Request, ACK, Selection, internal activation/feedback, external
  event, End event, and terminal Response paths MUST preserve NDNSF permission,
  ABE-backed service-semantic authorization, UserToken, ProviderToken,
  signature, replay, and provider-permission invariants.
- **FR-040**: Invocation-event confidentiality MUST use one bounded
  stream/session protection epoch established through the authorized control
  path, and the framework MUST NOT repeat an ABE bootstrap for every event. A
  Normal Request MUST carry only a commitment to the event key. After plan
  commit, the key MUST be wrapped to the selected final-role Provider and
  delivered through its Provider-specific Selection projection; unselected ACK
  Providers MUST NOT receive a decryptable grant. A selection-free Targeted
  request MAY carry a grant wrapped only to its explicit target certificate.
- **FR-041**: Every event MUST still be individually signed and protected by
  authenticated encryption bound to its exact name and lineage.
- **FR-042**: The publisher queue, consumer Interest window, reorder buffer,
  retained-event window, callback queue, and concurrent streamed-invocation
  count MUST be bounded and observable.
- **FR-043**: The baseline queue policy MUST apply backpressure to generation
  before overflow. Dropping an accepted token event, silently changing cursor
  numbering, or continuing unbounded generation is forbidden.
- **FR-044**: Logs and evidence MUST exclude plaintext prompts, decoded answers,
  logits, token encryption keys, and KV tensors by default; hashes, sizes,
  cursors, timings, identities, and terminal reasons are sufficient.

#### Compatibility, Observability, and Validation

- **FR-045**: Existing unary C++/Python APIs, Targeted behavior, streaming media
  facade, and Spec 174 complete-response semantics MUST remain source- and
  behavior-compatible unless an independently reviewed migration is added.
- **FR-046**: The implementation MUST reuse the existing verified-delivery
  attempt/plan/generation fencing, terminal-response guard, exact collaboration
  Data transport, and bounded scheduler primitives instead of creating parallel
  sources of truth.
- **FR-047**: The implementation MUST emit bounded timestamps for request
  creation, ACK closure, plan commit, Selection acceptance, preparation,
  prefill, each decode epoch, internal feedback publication/fetch, external
  event publication/fetch/delivery, terminal Response, cancellation, retry,
  and backpressure.
- **FR-048**: Evidence MUST distinguish implemented, wired, executed, measured,
  and performance-qualified states; a lower evidence level MUST NOT be reported
  as a higher one.
- **FR-049**: Unit tests MUST cover message/name encoding, lifecycle transitions,
  ordering, duplicate suppression, gap retry, terminal consistency, early
  completion, KV compatibility, security negatives, backpressure, and unary
  regression.
- **FR-050**: CPU-only integration tests MUST execute real signed event Data and
  the complete Request-to-final-Response path with a deterministic small ONNX
  model, including multi-role generation and injected loss/reorder/duplicate,
  cancellation, stale-attempt, replacement, unselected-key isolation, and a
  Provider-capability permutation that changes the ACK-driven role mapping.
- **FR-051**: Host/CPU MiniNDN tests MUST run the real controller, user, repository,
  NFD/SVS path, and two- and four-Provider role plans with a deterministic small
  ONNX model and fixed workload seed.
- **FR-052**: MiniNDN MUST demonstrate both an unchanged unary workload and a
  streamed LLM workload, and MUST prove through packet/evidence lineage that
  internal activation, token feedback, external events, and final Response use
  the declared Interest/Data paths.
- **FR-053**: The final local SIF build and TigerCluster submission MUST be gated
  on all unit, integration, security-negative, and host/CPU MiniNDN tests and on an
  immutable host-gate manifest plus one locally built SIF whose hash, Python ABI, native extensions, ONNX
  Runtime provider, linked libraries, runtime assets, workload, and relative
  paths pass the project native preflight. The qualifying SIF MUST embed the
  exact sealed framework, Python, adapter, and workload source used by G0-G4;
  runtime source overlays or bind-mounted replacement modules are forbidden in
  G4-G6. Large model artifacts remain external, immutable, and content-addressed.
  For G4, MiniNDN, Mininet, Open vSwitch, NLSR, `mnexec`, host networking tools,
  the topology, and the replay orchestrator MUST remain on the host. They MUST
  NOT be required inside the SIF. The host MUST launch the candidate's NFD,
  Controller, repository, Provider, User, Python extension, and ONNX Runtime
  entry points through `apptainer exec` inside the MiniNDN-created namespaces.
  The host-emulator manifest and SIF-runtime manifest are distinct and both are
  required; neither can satisfy the other's preflight.
  The pre-build gate MUST also reject any unclassified signal/native process
  exit from the current G3 attempt set; a later PASS rerun does not erase that
  failure. Before submission, a version-controlled **proven Tiger profile**
  MUST bind the exact wrapper/helper hashes, Apptainer invocation, bundle
  `cwd`, mounts, HOME/PIB/TPM isolation, identities, NFD routes, resource
  envelope, child-start order, readiness barrier, workload, and result oracle
  inherited from the successful Spec170 D0/r23 route. A machine-readable,
  field-by-field delta record MUST compare the fully rendered candidate command
  and effective configuration with that profile. Only the gate-specific
  allowlist (candidate/SIF identity, run/output identity, model/artifact
  identity and mount, and (for stage-readiness only) the explicitly consumed
  stage-device mapping) may differ. Provider/GPU counts, seed, and the frozen
  gate-specific memory/walltime envelope are fixed per gate; changing them
  requires a new proven profile/job and source seal. An unknown field,
  ambient environment value, changed helper/config byte, or undeclared delta
  MUST fail before SSH, upload, staging, or `sbatch`; prose justification alone
  is insufficient.
- **FR-054**: TigerCluster qualification MUST use the same signed workload and
  contract as local gates, require CUDA ONNX Runtime for model computation with
  no CPU compute fallback (bounded int64/bool shape-control nodes may use the
  CPU EP), and
  separate cold preparation from warm measured generations. The primary subject
  is the pinned Qwen3.6-27B three-Provider ONNX plan. Generic one-shot
  compatibility is closed by the local unit and native integration gates; it is
  not duplicated as a separate expensive Tiger row. Any smaller-model smoke
  remains diagnostic only. Before the full
  multi-Provider invocation, each 27B stage MUST pass an exact-SIF CUDA ORT load,
  execution, digest, and node-local-cache readiness gate. Model staging time is
  reported separately and is excluded from generation latency. Before those
  27B gates, the exact current SIF MUST pass one bounded CPU/no-GPU Tiger
  deployment control shaped like the successful Spec170 D0 lifecycle. This
  control is deployment evidence only and cannot satisfy G5 or G6.
  G4T--G6 MUST be rendered from the same proven Tiger profile through one
  repository-owned submission entry point. Operators MUST NOT invoke a gate's
  `.sbatch` file directly, add an ad hoc wrapper, or rely on `--export=ALL` or
  an inherited shell environment to supply unregistered values. A later gate
  changes only the parameters registered for that gate; all common launcher,
  routing, identity, mount, readiness, and evidence settings remain byte- or
  field-identical to the passing current-SIF control.
  Stage readiness MUST allocate one distinct CUDA device to each complete
  pipeline stage; loading all three 27B stages into one GPU is invalid. Every
  declared state family's input and successor output MUST have the same tensor
  element type so a device-resident successor can be rebound without an
  implicit host cast or a hidden full-state round trip.
- **FR-055**: Every test or experiment run MUST record code commit, SIF/model/
  tokenizer/adapter/workload digests, topology, Provider-role mapping, runtime
  versions, seed, commands, timeouts, result counts, failures, and evidence
  hashes in a machine-readable manifest.
  The manifest MUST retain every attempted process, including setup failures,
  signal exits, and superseded diagnostics; selection of a three-PASS subset
  MUST NOT hide a failure from the same candidate/subject.
- **FR-056**: A performance report MUST include time to first token, per-token
  latency distribution, steady-state tokens per second, total latency, success
  and exactness, retransmissions/gaps, queue/backpressure, complete decode-state
  reuse/recompute,
  resource utilization, and CPU-fallback count.
- **FR-057**: Tiger failures MUST be reproduced by the smallest lower-cost gate
  that can exercise the same boundary before another full campaign is submitted.
  Deployment/configuration failures MUST first be compared with the registered
  historical-success control and reduced to exactly one declared delta. A
  failed current-SIF Tiger control blocks model staging and all larger jobs.
  A retry MUST reuse the same rendered profile and change only the single
  registered corrective parameter; changing a helper, environment name,
  resource request, path, timeout, model, and topology together creates a new
  subject and is forbidden as a retry.

#### Multi-Turn Conversation Continuation and Tiered State

- **FR-058**: The NDNSF-DI application contract MUST distinguish
  `FULL_CONTEXT` from `APPEND_DELTA`. Omitting conversation options MUST preserve
  the existing request-scoped `FULL_CONTEXT` behavior. `APPEND_DELTA` MUST be an
  explicit opt-in and MUST NOT be inferred from equal prompts or identifiers.
  The implementation and evidence MUST call the state used by prefill/decode
  inside one fresh Request **request-local decode state**, and the state retained
  after a successful turn for a later Request **conversation-scoped state**.
  A request-local cache hit MUST NOT be reported as cross-request continuation.
  These are two actual Provider-local state stores, not two names for one
  counter: request-local state is the live KV/recurrent/convolution bundle for
  the current Request, while conversation-scoped state is a promoted,
  retained KV/recurrent/convolution bundle that a later fresh Request may
  restore. The user-side transcript and opaque checkpoint contain only
  logical/authenticated commitments; neither one is itself a KV-cache or a
  substitute for Provider-local state readiness.
  The adapter MUST prove that canonical tokenization of the new complete
  transcript is exactly the committed parent token prefix followed by the
  computed appended canonical suffix. The caller supplies only the new
  application message in `APPEND_DELTA`; that suffix MAY also contain exact
  chat-template/control tokens derived by the sealed adapter. If this prefix-
  extension property does not hold,
  delta prefill is forbidden and only the registered full-context fallback or
  failure is allowed.
- **FR-059**: A conversation MUST have a stable `conversationId`; every turn
  MUST receive a fresh NDNSF request ID and generation ID and MUST identify its
  expected parent context epoch. Conversation identity MUST NOT replace
  request, attempt, plan, generation, or security identity.
- **FR-060**: The caller MUST receive and return one opaque, authenticated
  `ConversationCheckpointV1`. It MUST NOT contain raw state tensors, memory
  addresses, storage paths, or a caller-selected Provider list. The turn request
  MUST bind the checkpoint, mode, appended-input digest, and any authenticated
  full-context fallback to the new request contract. The User-side coordinator
  MUST keep a protected persistent `ConversationTranscriptRecordV1` containing
  the exact application-message lineage and canonical token IDs for each
  committed context epoch; the checkpoint binds its digest. This record is not
  sent to Providers during a healthy delta turn. If it is missing or cannot be
  decrypted after restart, only explicit full-context fallback or failure is
  allowed.
- **FR-061**: Cross-turn reuse MUST require the same one-to-one Provider-role
  assignment and exact compatible service, requester/security domain, model,
  graph, adapter, split, tensor layout, Provider identity, Provider boot epoch,
  cache epoch, parent context epoch, token-prefix digest/count, and position
  identity. A new turn remains one Request, one placement decision, and one
  automatic generation; a conversation checkpoint MUST NOT bypass normal
  authorization or silently authorize a different placement.
- **FR-062**: Each selected Provider MUST own only its complete role's
  conversation state. State tensors MUST remain Provider-local and MUST NOT be
  carried in Request, ACK, Selection, activation, token-feedback, event, or
  Response packets. NDN may carry only signed commitments, role receipts,
  readiness, state-tier hints, and control metadata.
- **FR-063**: A reusable conversation checkpoint MUST be committed
  transactionally across all selected roles. Every role receipt MUST name the
  same conversation, parent and successor context epochs, common logical
  prefix, request contract, plan, and role map. The coordinator MUST NOT expose
  the successor checkpoint until every role accepts; missing or mixed role
  state MUST never be partially reused. For a conversation-enabled turn, the
  application success result and successor checkpoint MUST become observable
  together only after this aggregate commit; an End/Response received earlier
  remains internal pending state rather than an application success callback.
  Before producing a receipt, each role MUST prove that its promoted state
  represents the exact canonical completed-turn prefix, including every
  accepted non-EOS assistant token and any sealed turn-finalizing template
  suffix. If the terminal sampling step left a bounded suffix unconsumed, the
  adapter MUST execute `CHECKPOINT_FINALIZE` state-only transition(s) with no
  sampling, event, or callback. Version 1 seals
  `maxCheckpointFinalizeTokens=32`; a larger or non-prefix finalization need
  MUST fail conversation promotion and use only the explicit full-context path
  on a later turn. A zero-copy ownership transfer from the
  request-local store is allowed, but merely preserving or re-keying a stale
  request-local entry is not. A cross-request cache hit is reportable only
  when the second fresh Request restores the promoted Provider-local state and
  performs delta prefill over the authenticated appended suffix. Repeating the
  full transcript, finding only a transcript/checkpoint, or incrementing a
  cache-hit counter without consuming the promoted state is not a
  conversation-scoped hit.
- **FR-064**: The Provider state manager MUST track storage tier separately
  from lifecycle: residency is `GPU_RESIDENT`, `HOST_RESIDENT`, or `EVICTED`,
  while lifecycle is `IDLE`, `PREFETCHING`, `PINNED`, or `COMMITTING`.
  Selected active state MUST be pinned on the execution device. Inactive state
  MAY move to bounded host RAM, and host-resident state MUST be prefetched
  asynchronously before execution. Model weights remain device resident and
  are not part of conversation-state movement. Residency transitions MUST
  correspond to actual adapter-owned device/host allocations and measured
  D2H/H2D transfers; changing an enum and byte counters without moving state is
  invalid.
- **FR-065**: GPU and host conversation-state stores MUST have independent
  configurable byte and entry quotas, deterministic inactive-LRU eviction, a
  bounded default retention of 300 seconds, and single-flight prefetch per
  state entry. Active, committing, and prefetched-for-dispatch entries MUST NOT
  be evicted. Disk/NVMe spill is not part of this feature.
  Eviction and invalidation MUST release/zeroize adapter-owned host/device
  buffers according to the existing trusted Provider runtime policy. G6C MUST
  verify those physical allocations, transfers, and releases with runtime copy
  counters; CPU tier emulation proves lifecycle logic only.
- **FR-066**: If exact continuation state is unavailable, the runtime MUST
  either (a) perform a recorded full-context prefill when the caller supplied an
  authenticated full context and explicitly allowed fallback, or (b) terminate
  with a specific continuation failure. It MUST NOT use zero-filled, stale,
  partial, cross-conversation, or cross-role state.
  A full-context fallback MAY perform normal ACK-driven placement on a different
  Provider-role map, but it MUST be labelled `FULL_PREFILL_FALLBACK`, MUST NOT
  claim state reuse, and its successor checkpoint MUST bind the new map.
- **FR-067**: Version 1 conversation history MUST be linear and single-writer.
  Committing a successor MUST compare-and-swap the expected parent context
  epoch. A second concurrent successor from the same parent MUST fail with an
  explicit conflict; implicit branching and merging are out of scope.
- **FR-068**: Conversation checkpoints and role receipts MUST be authenticated
  and bound to requester identity, unified service name, security domain,
  model/plan identity, role, Provider identity, boot/cache epochs, prefix
  commitment, and expiry. Possession of `conversationId` alone MUST grant no
  access. Logs and manifests MUST NOT expose prompt text, state tensors, wrapped
  secrets, or raw checkpoint capability material.
- **FR-069**: Every continuation attempt MUST report checkpoint validation,
  per-role hit/miss and reason, source and destination residency tier, resident
  bytes, prefetch queue/latency/bytes, eviction, wait time, delta-prefill token
  count, full-prefill fallback, and final checkpoint commit status. A claimed
  hit MUST be paired with measured avoided-prefix work.
- **FR-070**: Conversation continuation MUST pass deterministic unit, process
  integration, and real MiniNDN tests before the single final SIF is built. The
  local matrix MUST cover at least three isolated two-turn conversations,
  1/2/4-role plans, GPU-tier emulation, host-tier restore, missing/expired/
  forged/mismatched state, Provider restart, full-prefill fallback, concurrent
  parent conflict, cancellation during prefetch, and zero state-tensor bytes on
  NDN edges. Actual GPU-to-host-to-GPU movement is proven only in the final CUDA
  qualification and is not inferred from CPU tests.
- **FR-071**: Request-local and conversation-scoped state MUST have separate
  lifecycle authorities. `ProviderDecodeStateEntryV1` is indexed by the fresh
  request/attempt/plan/generation identity and is terminally released unless it
  enters a conversation promotion transaction. `ConversationStateEntryV1` is
  indexed by the authenticated conversation/context epoch plus exact role,
  model, placement, Provider, boot/cache, prefix, and security identity and may
  outlive the originating Request. Promotion MUST be an atomic ownership
  handoff: freeze the completed transcript, finalize the exact prefix if
  needed, stage one candidate per role, collect every receipt, compare-and-swap
  the parent epoch, then release the request-scoped owner. Promotion failure
  releases the new candidates and leaves the previously committed parent
  conversation state usable; direct lookup of a terminated request-local entry
  by a later Request is forbidden.

#### Immutable Candidate and Pre-Dispatch Closure

- **FR-072**: One promotion candidate MUST be identified by one immutable tuple
  containing the source seal, exact SIF digest, host-replay harness digest,
  Tiger submit-bundle digest, workload/configuration digest, model/artifact
  digest, and validation-contract digest. A PASS from a different tuple is
  historical evidence and MUST NOT authorize promotion.
- **FR-073**: Every candidate change MUST be classified as `sealed-runtime`,
  `host-replay`, `tiger-submit`, or external `model-workload`. The candidate
  manifest MUST record the old and new identities, affected gates, and earliest
  restart gate. A runtime or host-replay change invalidates the corresponding
  lower gates and exact SIF replay; a submit-only change MAY reuse an unchanged
  SIF only after the submit closure and current-SIF control are rerun.
- **FR-074**: Before any Tiger network or scheduler side effect, one executable
  closure validator MUST resolve and hash every transitive wrapper/helper,
  producer and consumer environment variable, interpreter, argument, bundle
  working directory, artifact and sidecar, mount, identity/token/policy entry,
  per-process HOME/PIB/TPM path, Controller/service startup path, timeout, and
  resource request. Provider READY markers MUST be emitted only after native
  handler/filter registration and event-loop start have completed; a marker
  before that point is invalid readiness evidence. Missing, stale, ambiguous,
  or unclassified inputs MUST stop before SSH, upload, remote mutation, model
  staging, or Slurm allocation. The validator MUST also compare the rendered
  submission with the proven Tiger profile, reject every difference outside
  the gate allowlist, and emit an exact effective-configuration digest. The
  command passed to `sbatch` MUST be the command it validated; a separately
  reconstructed command is invalid.
- **FR-075**: Repository publication readiness MUST distinguish local
  catalog/permission/registration visibility from live service reachability.
  Before releasing the publication barrier, a bounded non-mutating probe from
  the same publisher identity MUST receive a validated ACK over the exact NFD
  route and record request, Provider, route/Face, send, ACK, and exit evidence.
  A bounded retry budget MAY be used for transient route startup, but every
  attempt MUST use a fresh request ID and be retained in the probe report.
  Failure MUST be reported as `REPO_SERVICE_ROUTE_NOT_READY`; a marker, sleep,
  unrecorded retry, or longer publication timeout MUST NOT satisfy this
  requirement.
- **FR-076**: A test or deployment case is terminally successful only when its
  protocol oracle passes, every required result file is freshly created, every
  child exits with the registered status, native teardown has no signal not
  initiated by the harness and no abort, and no owned process remains. The
  harness's explicit bounded SIGINT shutdown, or a bounded force-termination
  after graceful-drain expiry, is recorded as intentional teardown. A Response,
  success marker, benchmark line, or expected negative error MUST NOT hide
  failed cleanup or a missing result.
- **FR-077**: Tiger MUST NOT be used to discover defects covered by the closure
  validator or G0--G4. At most one active terminal subject is allowed for a
  candidate and gate; pending, canceled, superseded, or cross-candidate jobs
  MUST be classified and MUST NOT count as repetitions or PASS evidence.
- **FR-078**: The repository submission entry point MUST require a
  candidate-bound closure manifest whose checks and evidence hashes cover
  FR-072--FR-077. Mutation tests MUST prove that each registered historical
  deployment-failure class is rejected before network or scheduler side
  effects. Those mutations MUST include an unknown environment variable,
  direct `.sbatch` invocation, changed wrapper/helper byte, changed bundle
  `cwd`, and a non-allowlisted parameter delta from the proven Tiger profile.

- **FR-079**: A production native Provider MUST begin from the canonical ONNX
  artifact identity and sealed role recipe selected by the accepted plan. The
  canonical identity (`assignedArtifact`) and network transport reference
  (`artifactDataName`) MUST remain separate; the latter may be an encrypted,
  segmented large-data name. After Selection the Provider fetches that
  reference, verifies its payload digest, and installs it under the assigned
  canonical identity before assembling the local role. Only
  after Selection may it fetch the required canonical graph/initializer data,
  verify the recipe and artifact digests, assemble the Provider-local role
  artifact, sign/cache it under the accepted identity, and load the runner.
  The ACTIVE root MUST bind one separately addressable canonical source Data
  object through `canonicalSourceDataName`, `canonicalSourceDigest`, and
  `canonicalSourceBytes`; layer-object byte totals are not a valid source
  substitute. The Spec175 ensurer MUST publish the source and ACTIVE root
  through an authorized transport before the Selection publication barrier and
  MUST bind both references in the sealed plan, or fail closed when an
  already-authorized repository publisher supplies them.
  Startup-time ready-made role files and
  `allowPreassembledV3Compatibility=true` are diagnostic compatibility paths
  and MUST be rejected by the formal Spec175 workload. The production
  `runnerPreparationFactory` (or its single canonical successor) owns this
  operation; a Python helper that is not reached by the native Provider does
  not satisfy the requirement.
- **FR-080**: Production Core, Provider, coordinator, state, repository, and
  adapter diagnostics MUST use named `NDN_LOG` components and ndn-cxx severity
  levels so `NDN_LOG` environment filters control both verbosity and target
  components. Plain stdout/stderr is limited to bounded atomic operator-facing
  summaries and machine-readable result records. Logs retain FR-044 privacy and
  MUST NOT expose prompt text, decoded answers, state tensors, or secrets.

### Key Entities

- **Streamed Invocation**: One accepted service request with immutable request,
  attempt, plan, generation, stream-epoch, security, deadline, and lifecycle
  identity.
- **Invocation Event**: One signed, encrypted, cursor-addressed application
  progress item with exact lineage and payload digest.
- **End Event**: The final stream event declaring why emission stopped, the last
  cursor, token count, transcript digest, and terminal-result reference.
- **Terminal Response**: The sole authoritative complete application result for
  the invocation.
- **Generation Specification**: Sealed model, tokenizer, adapter, sampling,
  stop, deadline, role, and deterministic-workload parameters.
- **Decode-State Identity**: Exact compatibility key for provider-local
  incremental state, including full-attention KV and any model-required
  recurrent/convolution state.
- **Request-Local Decode-State Entry**: Provider-owned state used only by
  prefill and decode transitions of one request/attempt/plan/generation; it is
  released at terminal completion unless atomically promoted.
- **Conversation-Scoped Decode-State Entry**: Provider-owned KV/recurrent/
  convolution state retained after an all-role promotion for one committed
  conversation context epoch. It is usable only by a later fresh Request
  after checkpoint, placement, identity, and exact-prefix validation; its
  receipt/checkpoint metadata is not the state bundle itself.
- **Event Window**: Bounded publisher retention, consumer Interest, reorder,
  retry, and callback state for one stream epoch.
- **Event Key Commitment and Grant**: A signed Request commitment plus one
  certificate-wrapped envelope disclosed only to the final-role Provider's
  Selection projection, or to the one explicit selection-free Targeted Provider.
- **Qualification Manifest**: Immutable provenance and measurements for one
  local or Tiger gate.
- **Conversation**: Stable application-level lineage spanning multiple turns;
  it is an index and ownership scope, not an authorization token.
- **Conversation Turn**: One fresh NDNSF Request and generation that either
  starts from full context or resumes exactly one committed parent checkpoint.
- **Conversation Checkpoint**: Opaque authenticated commitment proving that all
  selected roles committed the same successor context epoch and logical prefix.
- **Provider Conversation-State Receipt**: Provider-signed role-local
  commitment to exact compatible state without disclosing its tensors or local
  storage address.
- **Conversation-State Entry**: One Provider-local role state plus exact
  identity, residency, pin/prefetch/commit lifecycle, quota accounting, and
  expiry metadata.
- **Conversation-State Promotion**: All-role transaction that converts the
  exact finalized terminal state of one conversation turn into the successor
  conversation epoch without making old request authority reusable.
- **Conversation Transcript Record**: User-side encrypted durable record of the
  committed application messages and canonical token sequence used to validate
  the next appended-input prefix; it contains no Provider-local model state.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In deterministic unit and integration cases, every accepted event
  cursor is delivered to the application exactly once and in order, with zero
  stale-attempt or post-cancellation deliveries.
- **SC-002**: Every successful streamed invocation produces exactly one End event
  and one complete terminal Response whose cursor count and transcript digest
  agree.
- **SC-003**: A fixed-seed small-model generation produces the same token IDs and
  finish reason as its local ONNX oracle in one-Provider, two-Provider, and
  four-Provider CPU validation cases.
- **SC-004**: Evidence for an N>=1-token healthy generation shows a Provider-list-
  free Normal application call, one unchanged request ID across discovery,
  placement, Selection, events, and terminal Response, one prefill transition,
  N-1 incremental decode transitions that consume only the new token/current
  activation plus exact compatible local
  decode state, and no second or per-token service Request/plan commit. The
  registered capability permutation changes the role map without changing the
  result oracle. Each selected Provider reports one automatic prefill-state
  commit followed by N-1 local state hits/atomic successor commits; the formal
  test contains no caller- or harness-managed state feedback and transports no
  decode-state bundle over NDN. For every inference epoch, all selected roles
  report the same logical token-prefix digest/count while reporting distinct
  role identities; changing activation bytes alone never changes or authorizes
  the logical prefix identity. Every decode hit names exactly the immediately
  preceding state epoch, and the runner-call count remains zero for a missing or
  mismatched predecessor. The eight-token CPU oracle also records one matched
  full-prefix reference, exact token parity, each cached runner input extent,
  and a positive machine-derived prefix-work-avoided total; a reported cache hit
  that still executes the full prefix fails this criterion.
- **SC-005**: All declared loss, reorder, duplicate, replay, tamper, timeout,
  cancellation, queue-pressure, and one-replacement cases either recover to the
  exact accepted transcript or terminate with the specified reason; none returns
  a silent partial success. An unselected or nonfinal Provider cannot unwrap the
  Normal event key or produce an accepted event.
- **SC-006**: The complete existing unary and Targeted regression suites pass
  unchanged after streamed invocation is enabled.
- **SC-007**: CPU integration and MiniNDN gates complete without PyTorch or
  Transformers in the deployed runtime; Tiger uses CUDA for model computation,
  and any CPU EP work is limited to the bounded int64/bool shape-control
  allowlist rather than model-compute fallback.
- **SC-008**: No Tiger job is submitted unless its manifest identifies passing
  G0-G4, no unclassified current-subject native exit, the promoted SIF hash, and
  the exact external model/cache manifest. Before G5/G6, one bounded current-SIF
  Tiger deployment control reproduces the registered D0-shaped lifecycle; every
  completed Tiger result points to those same identities and uses no runtime
  source overlay. The validated rendered profile reports zero undeclared
  differences from the proven Tiger profile, and the actual `sbatch` input hash
  equals the validated hash.
- **SC-009**: A functional Tiger run produces a nonempty, ordered token stream,
  exact final result, zero CPU model-compute fallback (bounded CPU
  shape-control is reported separately), one terminal Response, and complete
  TTFT/TPOT/resource evidence. Each stage reports persistent CUDA state bindings,
  one prefill and one-token decode inputs after prefill, monotonically advancing
  state length/epoch, zero full-cache host round trips on the healthy path, and
  bounded state allocation/cleanup. The evidence also binds each cache entry to
  the trusted static identity sources and records the common logical token
  prefix, role-local state epoch, predecessor epoch, position digest, and cache
  epoch without disclosing prompt tokens or state tensors. Local unary
  compatibility is checked by the G1/G2 production-path suites and is not
  counted as a streamed or performance result.
- **SC-010**: A Tiger run is labeled `PERFORMANCE_PASS` only when its warm
  steady-state median output rate is at least 20.0 token/s, its p95 inter-token
  interval is at most 75 ms, all correctness criteria pass, and a registered
  bounded same-artifact cached-versus-full-prefix control shows that incremental
  decode processes only the new token and has lower median model-compute time
  than full-prefix recomputation for the same generated prefix. Otherwise it is
  labeled `FUNCTIONAL_PASS_PERFORMANCE_MISS` with the measured bottleneck.
- **SC-011**: At least three independent warm Tiger processes are measured after
  one excluded cold preparation run; all seeds, run counts, failures, and
  dispersion are reported without discarding valid negative runs.
- **SC-012**: All high-impact API, wire, security, recovery-default, topology,
  workload, measurement, and verdict decisions are fixed before implementation.
  If a lower-level implementation detail is genuinely omitted, the implementer
  may choose the smallest solution consistent with these decisions and current
  code, and MUST record the rationale and closing test in the same task.
- **SC-013**: For every registered two-turn case, the second-turn token IDs,
  finish reason, and final text exactly match the full-transcript oracle. Its
  trace shows first-turn request-local prefill/decode, exact terminal-prefix
  finalization and all-role promotion, a fresh second Request/generation, a
  verified parent checkpoint, only the appended canonical suffix in second-turn
  prefill, one automatic decode loop, and positive measured prefix work avoided.
  The trace separately reports raw new-message token count, template/control
  suffix token count, and total delta-prefill token count.
- **SC-014**: At least three concurrent conversation identities remain isolated
  across active, host-resident, prefetched, and evicted states. Every selected
  role reaches one compatible ready state before execution; zero state tensor
  bytes appear on NDN edges and model weights never move with conversation
  state.
- **SC-015**: Stale, expired, forged, wrong-requester, wrong-service,
  wrong-Provider/boot/cache, wrong-model/plan/layout, incomplete-role, and
  concurrent-parent mutations, as well as a fresh Request attempting to address
  a terminated request-local entry directly, all produce the registered explicit
  fallback or failure with zero incompatible runner calls and zero application
  success callbacks.
- **SC-016**: The final CUDA functional qualification records at least one real
  `GPU_RESIDENT -> HOST_RESIDENT -> PREFETCHING -> GPU_RESIDENT` conversation
  transition with exact output parity, state bytes, transfer/prefetch latency,
  device identity, and bounded cleanup. No memory or latency benefit is claimed
  unless the corresponding measurement is present.
- **SC-017**: For every Tiger submission, one machine-readable closure report
  proves equality of all candidate-tuple identities, names the restart gate,
  reports zero stale or cross-candidate prerequisite manifests, and records the
  proven-profile ID plus the complete allowlisted configuration delta.
- **SC-018**: The pre-dispatch mutation suite rejects 100% of the registered
  missing-helper, environment-name, bundle-cwd, artifact, identity-set,
  shared-PIB, Controller-liveness, resource-envelope, stale-SIF, and
  marker-only repository-readiness mutations while observing zero SSH, upload,
  remote mutation, model staging, or `sbatch` calls. It also rejects 100% of
  unknown/non-allowlisted profile fields, direct `.sbatch` calls, ambient-only
  values, and changed wrapper/helper/config bytes.
- **SC-019**: Every accepted G3--G6 case has a fresh terminal result and all
  required child exit statuses; zero accepted cases contain an unclassified
  signal, missing result, surviving owned child, or success marker without
  terminal protocol and teardown evidence.
- **SC-020**: The production four-Provider tiny-ONNX case starts without
  ready-made role artifacts, performs post-Selection Provider-local assembly,
  executes one authenticated sampler and standalone tokenizer, emits oracle-
  matching text deltas and final text, and records zero complete-state host
  round trips after prefill on the CUDA variant.
- **SC-021**: A CUDA conversation qualification records nonzero matching D2H
  pause and H2D prefetch bytes for every moved role, the corresponding physical
  device/host allocation transitions, exact continuation parity, and bounded
  release/zeroization. Enum-only or accounting-only transitions fail.

## Assumptions

- Spec 174 verified-delivery invariants, including its independently specified
  TensorGroup/rank-role protocol, remain authoritative prerequisites. Spec 175
  validates streamed generation only for the one-Provider/one-complete-role
  pipeline baseline and does not modify or retire the Spec 174 tensor path.
- Streamed cross-Provider tensor parallelism, continuous batching across
  unrelated user requests, speculative decoding, live cross-Provider KV
  migration, and
  OpenAI-compatible HTTP/SSE service hosting are out of scope for this feature.
  An optional application gateway may translate the generic event API to SSE
  without changing Core protocol semantics.
- The first implementation publishes one application token event per cursor.
  Event micro-batching is deferred until measurements justify it.
- Bounded provider replacement is disabled by default and may be enabled only
  by an application request that permits clean recomputation.
- The first performance target applies to one active generation, not a
  multi-tenant continuous-batching server.
- Multi-conversation pause/resume is in scope, but batching unrelated active
  conversations into one model step is not. Version 1 schedules one turn at a
  time per conversation and permits only one linear successor of each parent.
- Cross-turn continuation reuses state only when the exact Provider-role map is
  still valid. If placement changes, the implementation performs the explicit
  full-context fallback or fails; it does not transfer KV state between
  Providers.
- Only conversation state may move between GPU and host RAM. Model weights stay
  loaded on the GPU. Disk/NVMe tiering and cross-node state migration are out of
  scope.
- Offline export may use model-specific tooling, but the canonical artifacts
  and deployed runtime remain ONNX-only.
- The deterministic small-model fixture is committed or content-addressed and
  small enough for repeatable CPU integration and MiniNDN execution.
