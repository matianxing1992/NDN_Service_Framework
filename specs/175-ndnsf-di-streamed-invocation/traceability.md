# Planned Traceability: NDNSF-DI Streamed Invocation

**Status**: T001--T022, T024, and T029--T033 satisfy their named acceptance
gates; 28 of 34 tasks are checked. The implementation queue, T020 G0/G1/G2,
and T022 host/CPU G3 qualification are closed. The current priority is T023's
exact-SIF/G4 candidate. T023 and T025--T028/T034 remain open
qualification/promotion work.
The previous G0/G1/G2 manifests,
30-process G3 subject, additional M09 signal-11 exit, and existing SIF
candidates are historical regression evidence, not active campaign work. After
With implementation frozen, one current-source G0--G3 sequence determines whether
any historical failure still reproduces. G4--G7 are unpassed. The next
valid boundary is not MiniNDN inside SIF: host MiniNDN/Mininet/OVS/NLSR must
create the topology while the exact SIF supplies NFD and all NDNSF/ORT
application processes through `apptainer exec`.

## Executable G0 baseline

T001's initial source census found that the draft TLV range
`0xF636..0xF657` was already owned by the existing Stream mapping/FEC contract.
Following the wire contract's explicit collision rule, all 36 Spec175 types were
moved together to the verified-free range `0xF661..0xF684`; the gate's mutation
test retains proof that a pre-existing owner is rejected. The refreshed gate
finds all 58 FRs and 12 SCs. The shared worktree is intentionally dirty, so
T020 uses the content-bound `spec175-source-seal-v1` subject for source inputs;
feature evidence remains separately bound by G0 document digests. The pre-
Face-fix sealed subject passed G0, and any source-input change requires
regenerating the seal and rerunning the gate after implementation closure. The
current source required a new promotion-valid G0 manifest. T020 produced the
corresponding G0/G1/G2 PASS inventories, and T022 now supplies the fresh 42/42
host/CPU G3 matrix. T023 is the next gate.

T002 is `executed`: `tests/python/test_spec175_cpu_fixture.py` regenerates the
committed vocabulary-32/hidden-8/four-block fixture twice byte-for-byte, checks
the one-/two-/four-role ONNX graphs, and runs all three token sequences through
real ONNX Runtime CPU incremental inference. Its state mutation negatives prove
that dropping either attention KV or recurrent/convolution state changes the
next-token result. This closes the fixture itself, not G2 or any application
integration claim.

T003 is `implemented` and `executed`: the collision-free TLVs, standalone
wire/name/digest types, real Request/Response embedding, fixed canonical
vectors, checked narrowing, explicit wrapped-key `STREAM-GRANT` envelope
semantics, and the negative matrix pass in the 11-case focused native suite
and existing message/security regressions. The grant's recipient/binding
authorization, transport, and security acceptance are completed by T005--T008;
T009's generic host Python boundary is closed by the real-process evidence
below. Current promotion manifests are stale only because source changed after
their pass, not because these mechanisms are missing.
Job 202864 remains a full-context 27B CUDA execution diagnostic because it used
`useCache=false` and did not establish a decoded stateful token oracle.

T004 is `implemented` and `executed`: one `StreamInvocationLifecycle` owner is
attached to an existing user `PendingCall` or provider pending key, repeated
attachment is idempotent, deferred collaboration cannot be invented by the
attachment call, and provider cleanup fences an active owner before erasure.
The lifecycle/queue, message, and focused attachment tests pass; see
`evidence/t004-lifecycle-20260822.md`. T005-T007 still own transport, delivery,
and authorization integration, so G1 remains open.

T005--T008 have `implemented` and focused-executed checkpoints: the real one-Provider
Normal/Targeted Core path
publishes and retrieves signed encrypted events through SVS/IMS, orders and
deduplicates delivery, retries one gap, closes End/Response, and contains the
registered generic callback/cancellation/capacity regressions. T009 also
implements the contract-level `StreamedInvocation` handle, async iteration,
awaitable result/cancel, status/metrics, decoder/options arguments, and the
callback-versus-iterator single-consumer guard. A real host NFD/Controller/
Python-Provider/Python-User regression proves iterator and callback delivery,
contained handler failure, and post-handler writer fencing. The 2026-08-23
host rebuild proves the exact CPython-3.8 import path and current framework/
Boost-1.71/NDN-SVS loader closure, and the real process regression proves
iterator/callback delivery, contained failure, and writer fencing. T009's
generic host boundary is closed. The diagnostic r30 candidate also passes the
exact CPython-3.10 in-SIF import/ldd/ORT/residue preflight and immutable
build-record check; this is T021 local-candidate evidence, not a reason to
reopen the generic API task or claim G1/G4 promotion.

T013--T018 have closed implementation and focused-execution boundaries:
the persistent tiny-ONNX state and one-plan
Python oracles pass, automatic V3 now seals the generation and adapter-owned
role-state contracts, the immutable P1/P2 workload is validated and source-sealed,
and the native `OnnxRuntimeModelRunner` executes the checked-in
two-role fixture across eight stateful token steps (see
`evidence/t013-stateful-onnx-native-20260823.md`). The Python generation loop
also fails closed when Core rejects event, feedback, or terminal publication;
its tokenizer/accepted-prefix mutation now occurs only after event admission.
Native streamed collaborations also route legacy unary publication through the
stream terminal, and DI selection capacity is released only after terminal
acceptance.

The 2026-08-25 complete-identity correction is now implemented in the current
source: one shared C++ `DecodeStateIdentityV1` reaches the Provider Store, the
trusted artifact/projection/runtime template is enforced, and the coordinator
binds canonical consumed-token prefix, position, and explicit state/predecessor
epochs. The current mutation and cache-effectiveness suites cover this
implementation boundary. Exact Qwen3.6 graph identity and CUDA residency remain
qualification evidence owned by T025; the I01-I03/I15 results remain local
implementation evidence rather than a Qwen3.6 performance claim.

T019 has an implementation and focused-execution checkpoint: attempt 1/2 is signed through
Core bindings, the production coordinator exposes one logical handle over at
most two fresh Normal collaborations, an attributed failed Provider is excluded,
and the exact recovery Request digest is sealed into the V3 plan/projections and
verified by Python/native Providers. The terminal Provider recomputes but does
not republish the committed prefix. Focused orchestration and provider tests
pass; controlled I12 and the live-transport I13 boundary are registered and
pass. The complete current-source G2 closure and broader fault matrix were
previously open under T020/T022; the current source-sealed G0--G2 and
42-process G3 closures now record those gates. T019 itself is checked because
its implementation and focused acceptance boundary is closed.
T014 has a native-harness checkpoint: the shared production C++ epoch coordinator and tiny-ONNX
harness register I01-I03/I15. Each passes three fresh processes through
Request/ACK/plan/Selection, per-epoch authenticated DATA_V1 activation/
feedback, ORT, eight exact token events, and one End/Response. The gate requires
clean completion of every selected Provider and proves the I15 ACK-driven role
permutation. See `evidence/t014-native-i01-20260823.md`,
`evidence/t014-native-i02-20260823.md`, and
`evidence/t014-native-i03-i15-20260823.md`. Automatic V3 now seals
TOKEN_FEEDBACK, operation stride/capabilities, generation limits/EOS/sampling,
and complete adapter-owned state I/O; the native Provider derives and runs the
coordinator from the authenticated projection. The production Python user
requests `useCache=true`/`TOKEN_STREAMING`, and the legacy Python Provider
rejects an incomplete stateful streaming contract. Focused Python/native and
integration regressions pass; see
`evidence/audit-production-stream-wiring-20260823.md`. A post-fix real M01 run
now binds the campaign and actual NDNSF Request to
`/spec175-M01-1750001`, delivers eight ordered events and a terminal response,
and closes four Provider spans. This closes T014/T015's implementation
boundary; the source-sealed repeated manifests are recorded by T020/T022. See
`evidence/implementation-closure-request-id-20260827.md`.
Native I04-I15 are registered and pass focused runs, including controlled I12.
The exact live-Provider transport I13 boundary now passes; generic
one-Provider stream tests remain prerequisites rather than substitutes for any
2-role tiny-ONNX fault case.

| Source intent | Requirements | Design authority | Tasks | Acceptance evidence |
|---|---|---|---|---|
| Preserve unary YOLO-style one-shot calls | FR-001, FR-045 | `contracts/api-contract.md` | T004, T008, T020 | G1 unary/Targeted regressions, I14; the G3 M01-M14 subject is streamed-only |
| Add local-LLM-like incremental output without a per-token request | FR-002, FR-003, FR-004, FR-005, FR-006, FR-021, FR-022, FR-023, FR-024 | `plan.md` Decisions 1/7; API, generation, and native epoch-coordinator contracts | T008..T015 | five-event API case, I01-I03, M02-M04 |
| Preserve dynamic discovery and one deferred collaboration | FR-002, FR-006, FR-021, FR-046 | API contract Normal/DI bridge; generation Section 1 | T001, T004, T007, T008, T014, T015 | no-Provider-list API case; one request-ID lineage; I15/M10 capability permutation |
| Keep one complete final result | FR-007, FR-016, FR-017, FR-018 | wire completion contract | T003..T006, T008, T010 | End/Response mutation matrix and exact transcript oracle |
| Use deterministic exact NDN event names | FR-009, FR-010, FR-011, FR-012, FR-013, FR-014, FR-015 | `contracts/wire-protocol-v1.md` | T003, T005, T006, T016 | exact name vectors, Interest/Data lineage, loss/reorder cases |
| Avoid predictive mapping/FEC for request events | FR-010, FR-019 | `research.md` Decisions 2/6 | T003, T006, T022 | no mapping/FEC wire in packet lineage |
| Prefill once and automatically reuse exact complete Provider-local decode state | FR-021, FR-022, FR-031, FR-032, FR-032a | plan Decisions 8/8a; generation contract Sections 4/9; `native-epoch-coordinator-v1.md`; data model 10/10a/10c | T011, T013, T014, T020, T025 | production `executeRoleAsync` prefill commit/hit/atomic successor/cleanup; trusted static identity; common logical token-prefix/count/position lineage across roles; explicit state/predecessor epochs; no harness-managed state feedback; no state on NDN edges; complete mutation matrix with zero runner calls after rejected lookup; matched CPU cached/full-prefix semantic control with actual extents and positive work avoided; G5 matched CUDA timing/device-residency control |
| One Provider per complete role; retain PreSplitFirst | FR-023, FR-024, FR-025, FR-026 | generation contract Section 1 | T014, T022, T026 | plan/Selection role map and 1/2/4 CPU + 3-role Tiger lineage |
| Support EOS/stop/max/deadline/cancel correctly | FR-027, FR-028, FR-029, FR-036 | generation contract Sections 2/3/7/8 | T011, T012, T018 | early-EOS/stop/max transition and Unicode stop tests |
| Deployed ONNX-only stateful runtime and deployment-oracle separation | FR-030, FR-030a | plan Technical Context and Decision 8a; validation G3/G5/G6 | T012, T013, T021, T025, T026 | import/dependency closure, complete prefill/decode state I/O, explicit PyTorch-reference versus CUDA-ORT deployment oracle, persistent device state, zero full-cache host round trips, paired cached/full-prefix stage evidence, and actual ORT provider evidence |
| Default no ambiguous re-execution; optional bounded replacement | FR-033, FR-034, FR-035, FR-036, FR-037, FR-038 | research Decision 8; generation Section 10 | T011, T017..T019 | I12 opt-in success, I13 default failure, stale-attempt negatives |
| Explicit multi-turn API with fresh turn authority and exact same-placement resume | FR-058, FR-059, FR-060, FR-061, FR-071 | plan Decision 8b; API conversation section; data model 10a/10d/10d.1/10g | T013, T029, T030, T032 | ACK-finalized role map produces one authenticated `ConversationTurnBindingV1` in every Selection; later turns also carry one exact role-local state reference; separate request-local and conversation lifecycles; terminal-prefix finalization/promotion; I16/I17 and M11 exact delta-prefill/full-transcript parity; no public Provider/state field |
| Transactional all-role checkpoint and Provider-local receipts | FR-062, FR-063, FR-067, FR-068 | data model 10d.1/10e/10f; wire Section 2a; generation Section 10a | T029, T030, T032, T033 | first-turn and resumed-turn Selection-binding mutation matrix; exact finalized-role handoff; complete receipt mutation matrix; all-role commit/rollback; one concurrent successor; zero state tensors on NDN |
| Bounded GPU/host conversation state and explicit fallback | FR-064, FR-065, FR-066, FR-069 | plan Decision 8b; data model 10g; generation Section 10a | T031..T034 | unit tier state machine; I18-I20/M12-M14; G6C real GPU-host-GPU transition and unavailable-role fallback pair |
| Preserve authorization and event confidentiality | FR-039, FR-040, FR-041 | wire Sections 2/6-8 | T003, T005, T007, T017 | commitment/grant/signer/key/token/policy/replay/AEAD matrix; unselected Provider cannot unwrap |
| Bound queues/resources, contain callbacks, and use backpressure | FR-008, FR-020, FR-042, FR-043, FR-044 | data model states/metrics | T004..T006, T018 | callback-failure and capacity-1 slow-consumer cases; no-drop/high-water evidence |
| Reuse verified-delivery owners and emit honest evidence | FR-046, FR-047, FR-048, FR-055, FR-056, FR-057 | plan Architecture 9; experiment plan | T001, T010, T015, T020..T028 | manifests with implemented/wired/executed/measured labels |
| Unit and CPU integration before network | FR-049, FR-050, FR-070, FR-071 | validation G1/G2 | T002, T003..T020, T029..T032 | complete unit binary and I01-I20 process manifest, including terminal promotion and direct request-cache reuse negatives |
| Host/CPU MiniNDN before SIF | FR-051, FR-052, FR-053, FR-055, FR-070, FR-071 | validation G3 | T022, T023, T033 | fresh 42/42 four-Provider M01-M14 matrix, terminal-state promotion, fresh-request continuation, and packet lineage without SIF; all same-subject native exits retained and classified before promotion |
| Final SIF replay and promotion | FR-053, FR-055, FR-057 | validation G4 | T021, T023, T024 | one final SIF; separate host MiniNDN/Mininet/OVS/NLSR and in-SIF NFD/NDNSF/ORT preflights; host-orchestrated replay with per-node `apptainer exec`; no in-SIF MiniNDN or host runtime substitution |
| Historical-success delta and current-SIF Tiger control | FR-053, FR-054, FR-055, FR-057 | plan Architecture 0a; validation G4T | T024 | machine-readable comparison with Spec170 D0/r23 plus one bounded CPU/no-GPU Request/four-ACK/Selection/Response control before model staging |
| Final Tiger CUDA functional, conversation residency, and 20 token/s qualification | FR-022, FR-030..FR-032a, FR-053..FR-056, FR-064..FR-071 | validation G5-G7; experiment plan | T024..T027, T034 | G4T deployment control, G5 27B device-resident readiness, G6 exact streamed functional, G6C real conversation-state tier/fallback control, G7 unfiltered 30-unit verdict |

## Success criteria map

| Criteria | Closing task/gate |
|---|---|
| SC-001..SC-002 ordered delivery and terminal closure | T003..T010, G1/G2 |
| SC-003 deterministic 1/2/4 role oracle | T002, T014, T022, G2/G4 |
| SC-004 one Request/placement/prefill plus automatic Provider state reuse | T007, T011, T013..T015, T020, G2/G4/G6 |
| SC-005 fault/recovery and grant isolation | T007, T016..T019, I04-I13, M02-M09 plus unselected/nonfinal unwrap negatives |
| SC-006 unary/Targeted compatibility | T008, T020, G1/I14; G3 is not the unary compatibility subject |
| SC-007 CPU/no-runtime-Transformers | T012, T022..T023, G3/G4 |
| SC-008 one promoted SIF and current-SIF Tiger control | T021..T024, G3/G4/G4T/Tiger preflight |
| SC-009 Tiger functional stream and device-resident state continuity | T025..T026, G5/G6 |
| SC-010 cache-effectiveness plus end-to-end performance verdict | T025, T027, G5/G7 |
| SC-011 repetitions | T026..T027, G6-G7 manifests |
| SC-012 high-impact decisions fixed; omitted low-level choices documented/tested | T001 contract gate and T028 closure |
| SC-013 two-turn exact delta-prefill continuation | T013, T029, T030, T032, T033, I16/I17, M11; request-local decode, terminal-prefix finalization/promotion, then fresh-request continuation are separately evidenced |
| SC-014 multi-conversation/tier isolation | T031..T034, I18, M12, G6C |
| SC-015 checkpoint/fallback/conflict negatives | T029..T033, I19/I20, M13/M14 |
| SC-016 real CUDA-host-CUDA state transition | T034, G6C |

## Evidence-level rule

Before execution every row is `proposed`. Future closure updates each row only
to one of `implemented`, `wired`, `executed`, `measured`, or
`performance-qualified`, with an exact path and hash. A checked task without its
required run remains at most `implemented` or `wired`.
