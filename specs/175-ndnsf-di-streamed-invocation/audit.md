# Spec 175 Audit: Streamed Invocation and Conversation Continuation

**Audit date**: 2026-08-27  
**Scope**: `spec.md`, `plan.md`, `tasks.md`, `research.md`, `data-model.md`,
`quickstart.md`, `experiment-plan.md`, `traceability.md`, all versioned
contracts, and current implementation seams inspected through CodeGraph.  
**Document verdict**: **CONDITIONAL PASS**  
**Current implementation/promotion verdict**: **BLOCK (formal qualification and promotion)**

## Re-audit correction after the Experimental checkpoint

The 2026-08-27 checkpoint exposed two reproducibility defects that invalidate
the earlier promotion sequence without invalidating its diagnostic results:

1. the repository-owned Spec175 contract gate, Tiger checklist validator, and
   their focused tests were not included in the pushed checkpoint; and
2. the newest passing G1/G2 manifests were generated after the source seal used
   by the 42/42 G3 matrix. G1/G2 and G3 therefore describe different subjects.

Those gate files are restored to the versioned subject and their focused tests
pass 26/26. T020 has now reclosed on revision `fe0b09fc` with clean G0, native
unit 600/600, Python 219/219, and G2 38/38 bound to one source seal. T022 remains
open. The old G3 matrix remains strong historical regression evidence, but it
cannot authorize a SIF. The required order is now: rerun the unchanged 42-case
G3 matrix against the T020 seal, and only then enter T023/G4.

## Executive finding

Spec175 now contains a coherent end-to-end design for both the original
request-scoped token stream and the newly requested multi-turn conversation
path. The conversation design is additive: callers without conversation options
retain the current full-context behavior. A later turn uses a stable
conversation identity but fresh Request/plan/Selection/generation authority,
an opaque authenticated parent checkpoint, protected User-side transcript
state, exact same Provider-role placement, Provider-local role state, delta
prefill, and the existing automatic autoregressive decode loop.

The documents and host/CPU implementation are close to the final SIF path, but
the feature is not ready to claim completion, build a candidate, or promote a
SIF. T020 is closed again, but T023 remains blocked until T022 closes under the
same source seal. The current
source exposes the additive conversation API, protected checkpoint transaction,
Provider-local state stores, the native receipt/control seam, and M11--M14
host-gate entry points. This audit records the remaining production gaps that
must stay visible:

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
2. CUDA state outputs were indexed under `*_out` while the next epoch looked up
   `*_in`. That mapping and adapter-owned causal position materialization are
   now fixed and regression-tested, but the exact Qwen graph remains
   unqualified and the CUDA path still copies complete state to host for the
   current transaction representation. The Python adapter now exposes a
   matching position-input materializer and rejects orphaned state mappings,
   but complete graph-derived Qwen position wiring remains open; and
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
   source-sealed repetitions are retained as historical evidence and must not
   be substituted for the required same-seal T020/T022 rerun.

The 2026-08-26 source-seal-v5 G0--G2 manifests (I01--I20, 38/38) remain useful
development evidence, but the code correction above changed the source subject.
They are historical rather than the final T020 seal. M11--M14 have host/CPU
real-Provider checkpoints, and the previous T022 subject classified all 14
cases across 42/42 fresh processes. Later source corrections mean that result
must be repeated before promotion. The earlier M09 native signal exit did not
reproduce in that historical subject.

## Audit inventory

| Item | Result |
|---|---|
| User stories | 5 |
| Functional requirements | 73 identifiers, including FR-030a and FR-032a |
| Success criteria | 16 |
| Tasks | 34 total; 27 accepted, 7 open |
| Requirement traceability | 73/73 mapped |
| Structural audit | PASS |
| Spec Kit prerequisites | PASS |
| Whitespace/patch integrity | PASS |
| New TLV source census | No non-Spec use of `0xF685..0xF698` found; executable G0 update remains T029/T032 work |

### Verification snapshot (2026-08-27)

The current source passes the focused, native, and host/CPU qualification checks
completed so far:

- `./build/unit-tests`: 600 cases, no errors detected; the focused
  `Spec175*,ConversationState*` selection also passes all 33 cases.
- The current focused streaming/conversation/provider regression:
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
- The real MiniNDN launcher now removes only explicitly named, unowned stale
  `/run/nfd/<node>.sock` entries after `nfd-stop`; active listeners are a hard
  error. The stale/active safety regression passes (35 cases in the focused
  launcher gate).
- `tests/python/test_spec175_conversation.py`: 36 passed (including failed
  prefetch recovery, stale-flight cancellation, and Provider-boot cleanup).
- strict Spec Kit structural audit: PASS (73 requirements, 34 tasks, 27
  accepted tasks).
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
- After the receipt-scope correction, the current source was resealed and the
  native G2 runner passed all 20 registered I01--I20 cases once with no missing
  case. The immutable source-seal, contract-manifest, and G2-manifest hashes
  are recorded together in
  `evidence/current-g2-one-repeat-integrity-20260827.md`; this is still below
  the required three healthy repetitions.

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
boundary. T030 and T031 remain open for those boundaries.

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
| A175-01 | CLOSED (implementation); FORMAL REPLAY OPEN | The native readiness/receipt/control seam is implemented and the historical 42/42 matrix exercised the four-Provider Ready/receipt/commit paths, fresh Requests, exact later-request lookup, delta prefill, and atomic conversation epochs. The current committed subject still needs the same-seal replay. | T020/T022 |
| A175-02 | HIGH | CUDA `*_out -> *_in` mapping and adapter-owned causal position materialization are implemented, and the streamed adapter suppresses per-token complete-state host copies. The explicit request generation identity now reaches the V3 contract and readiness record; terminal/error cleanup now removes the session-keyed device state. The exact Qwen3.6 CUDA graph and the unary multi-role transaction's zero-host-round-trip proof remain unqualified. | T025 |
| A175-03 | HIGH | The prior host/CPU matrix contains exactly three fresh PASS processes for every M01--M14 case (42/42), but it does not share the post-fix G1/G2 source seal. | T020/T022 same-seal rerun |
| A175-04 | BLOCKER | No exact final candidate includes the corrected source; no G4/G5/G6/G6C/G7 evidence exists for it. | T023, T025--T027, and T034 |
| A175-05 | CLOSED (local implementation) | The production Python User drives the real four-Provider native path through the model/task-first streamed API; the historical matrix repeated it, while current formal replay remains T022. | request-ID regression; T022 |
| A175-06 | MEDIUM | Appended-input tokenization must be proved prefix-stable for the exact Qwen tokenizer/chat template; a delta assembled only from message text is insufficient. The generic/tiny implementation boundary is closed, but the exact Qwen qualification remains open. | T025/T034 |
| A175-07 | MEDIUM | Actual GPU-host-GPU state movement cannot be established by CPU tier emulation. | T034/G6C |
| A175-08 | CLOSED | The flaky `ConversationStateStoreSeparatesCrossRequestState` test observed transient PREFETCHING state after waiting for completion. It now asserts the final single-transfer state and passed 100 repetitions. | native unit regression |
| A175-09 | LOW | T029--T034 IDs were appended after older evidence references, so visual task-number order differs from execution order. | Follow the dependency graph; do not execute file order |
| A175-10 | CLOSED (local regression) | Production conversation checkpoints use purpose- and identity-separated journal-derived authentication keys; bounded previous-key verification and real Provider-signed Ready/receipt Data passed the historical M11--M14 matrix. | Python security regression; T022 replay |
| A175-11 | CLOSED (historical replay); CURRENT REPLAY OPEN | M11--M14 formal mode fails closed on test-only emulation, and all 12 historical real Provider conversation processes passed the receipt, promotion, commit-ack, lookup, isolation, restart, and cancellation schema. | T022 same-seal replay |
| A175-12 | CLOSED (local transaction); CURRENT REPLAY OPEN | The production coordinator previews/signs the exact checkpoint, publishes role-bound COMMIT controls before persistence, and rolls back on failure; historical M11--M14 execution confirmed the network path. | local transaction regression; T022 replay |
| A175-13 | CLOSED (local boundary) | The native staged-promotion commits now reject a non-empty binding checkpoint on the legacy no-checkpoint overload, and the authenticated two-argument overload rejects a non-empty binding checkpoint that differs from the aggregate checkpoint supplied by the COMMIT control. This closes caller-mismatch paths without changing the staged binding shape used by the current control protocol. | `ConversationStateStoreResolvesOnlyExactSelectionCommitment`; native unit regression |
| A175-14 | CLOSED (local boundary) | Both V2 and V3 `AutomaticInferenceHandle` conversation metadata now carry the exact sealed `plan_digest` consumed by the asynchronous promotion path. A completed streamed turn can therefore pass the plan-binding check instead of failing after the final response with a missing metadata field. | Spec175 Python/API regression (174 passed) |
| A175-15 | CLOSED (host/CPU boundary) | The Python Provider-state bridge restores failed prefetches to a retryable state, fences stale cancelled workers, and releases aliased state views during Provider-boot invalidation. Native Provider binding clears stale staged-promotion side-index entries and rejects mismatched or unauthenticated binding digests. Repeated M11--M14 closes real Provider-process ownership on CPU; CUDA residency remains A175-02/T034. | conversation regression; T022 G3 manifest |
| A175-16 | CLOSED (local regression) | The four-Provider production-ingress test incorrectly used asynchronous ACK vector position as Provider identity and then dereferenced a missing role-map iterator after a failed assertion. Selection role construction is now Provider-name keyed and the negative assertion is non-fatal. The candidate-only V3 role-spec helper remains source-compatible, and literal `"cpu"` device identities are rejected while CPU remains represented by an empty device tuple. | `evidence/current-local-regression-20260827.md`; focused Python 203 passed; native unit 600 and integration suites pass |
| A175-17 | CLOSED (local boundary) | ConversationStateStore now wipes every retained TensorBundle payload before eviction, expiry cleanup, Provider-boot invalidation, or explicit clear. This satisfies the local state-release/zeroization requirement without claiming cross-process or CUDA qualification. | native unit `*ConversationState*` (6 cases) |
| A175-18 | CLOSED (local boundary) | Reserved User COMMIT/ROLLBACK collaboration controls are now recipient-filtered by the Provider name encoded in the collaboration Data name; ordinary Provider-to-Provider data keeps the original User requester convention. | `ServiceProvider::onCollaborationDataMessage`; full integration suite |
| A175-19 | CLOSED (local execution boundary) | A lineage-bearing coordinator epoch could previously enter a runner's complete `runStreamed()` loop, duplicating token events and advancing state more than once per epoch. The worker now selects one-shot `run()` for coordinator-owned epochs, and standalone ONNX `runStreamed()` rejects authenticated lineage rather than reusing stale prefix positions. | `ProviderRoleWorker`; `OnnxRuntimeModelRunner`; native unit 600; integration suite |
| A175-20 | CLOSED (local evidence boundary) | Conversation continuation restored a promoted parent state but reported zero `prefixWorkAvoided` at request epoch zero. The coordinator now records the exact promoted parent prefix length while keeping conversation and request-local hit metrics distinct; the three-token parent regression reports three avoided tokens. | `NativeEpochCoordinator`; `NativeEpochCoordinatorRestoresConversationStateAndExtendsPrefix`; native unit 600 |
| A175-21 | CLOSED (launcher hygiene; execution still privilege-gated) | `nfd-stop` can leave a per-node Unix socket path behind. The Spec175 launcher now probes only the exact topology-owned paths, removes a path only after an unowned connection refusal, and rejects an active listener. When an unprivileged launcher cannot unlink a root-owned stale path it records an explicit deferred cleanup instead of claiming removal; a root MiniNDN start remains required for execution. | `cleanup_unused_nfd_sockets`; focused real-MiniNDN launcher regression (35 passed) |
| A175-22 | CLOSED (historical host/CPU observation) | `AutomaticStreamingHandle` records monotonic event/terminal/error timestamps and exposes a metadata-only `timing_summary`; the launcher emits strict per-role Provider handler spans. The prior T022 subject repeated the real native-Provider timing evidence across M01--M14; current replay remains open and performance qualification remains T027. | timing-summary regression; T022/T027 |
| A175-23 | CLOSED (local fail-closed boundary) | Direct `ConversationCoordinator.commit_turn()` callers now receive the same origin request/generation/service/requester/security-domain checks as `prepare_checkpoint()`, so a mismatched receipt cannot bypass the normal lineage contract. `AutomaticStreamingHandle` also contains user error-callback exceptions so they cannot escape the delivery thread or replace the original terminal error. | `conversation.py`, `app_sdk/placement.py`; correction regression 71 passed |
| A175-24 | CLOSED (local authorization-scope boundary) | `prepare_checkpoint()` and `commit_turn()` now require every receipt to share one non-empty service, requester identity, security-domain digest, and per-role model digest. A coordinator with blank constructor scope can no longer let the first receipt implicitly authorize a mixed receipt set. | `conversation.py`; `test_checkpoint_rejects_mixed_receipt_scope_when_coordinator_scope_is_blank` (3 parameterized cases) |

## Frozen execution order

```text
T013 complete state/position/device source wiring
  -> T014 automatic multi-role production loop [complete]
  -> T015 real Python-user/native-Provider workload and spans [complete]
  -> T019
  -> T029 contract/API [complete]
  -> T030 production transcript/checkpoint/delta-prefill transaction [complete]
  -> T031 production Provider conversation-state acquire/promotion [complete]
  -> T032 G0/G1/G2 and pre-frozen G6C gate implementation [complete]
  -> T033 M11-M14 real-Provider harness implementation [complete]
  -> T024 pre-frozen Tiger interface/checklist implementation [complete]
  -> T020 coherent committed-source G0/G1/G2 seal (I01-I20) [complete]
  -> T022 same-seal G3 replay (M01-M14, 42/42, no hidden exit) [open]
  -> T023 one final SIF and exact-SIF 42-case replay
  -> T025 G5
  -> T026 G6
  -> T034 G6C
  -> T027 G7
  -> T028 closure
```

No SIF build, upload, Slurm allocation, or Tiger run is authorized before the
same-seal expanded G3 pass. T032 has frozen the lower-gate/G6C launcher
contracts and T033's harness implementation is complete. T020 now has a clean
current-source PASS inventory; T022 must regenerate the matching G3 inventory,
while T014/T015/T030/T031 remain closed implementation boundaries.

## Final audit decision

Spec175 is sufficiently explicit to finish local qualification, but not yet to
enter the final SIF/promotion path. T014--T015 and T030--T031 close their
implementation boundaries, and T024's pre-frozen interface/checklist boundary
is complete. Do not reuse either the historical 30/30 result or the now-stale
42/42 result as the current expanded G3 pass. T020 is now closed on one
coherent committed source. The next action is T022 against that exact seal,
and only then T023 to build exactly one final SIF and run the exact-SIF replay
before Tiger.
