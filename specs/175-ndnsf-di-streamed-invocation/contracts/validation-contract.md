# Validation Contract: Spec 175 Promotion Gates

## Global rules

- Gates run in order G0 through G4, G4T, G5, G6, G6C, then G7. Any mandatory failure blocks every later
  gate; no `--force` path exists for evidence acceptance.
- Every process uses fixed workload seed `1750001` and Greedy sampling for exact
  token comparison. Fault scheduling uses fixed seed `1750002`.
- Every gate writes `qualification-manifest-v1.json` and a SHA-256 evidence
  index. Logs are bounded and exclude plaintext prompt/answer/KV/logits.
- Required tests may not be skipped. A missing dependency is a failed gate, not
  a skip/pass.
- Unless a gate-specific table overrides this rule, healthy deterministic cases
  run in three fresh OS processes and fault cases run once. The G3 table below
  explicitly overrides it: every M01--M14 case runs in three clean processes.
- The first full-model Tiger invocation is cold preparation and is recorded but
  excluded from warm latency distributions. Three fresh warm processes are
  mandatory.
- No runtime parameter may change between registered repetitions. A changed
  parameter creates a new campaign ID and cannot be pooled with the old one.
- **Implementation-first boundary**: while any Core/runtime/API/conversation or
  harness/launcher implementation task is open, only its focused
  compile/unit/integration checks may run. Formal G0--G3 manifests are not
  repeatedly regenerated during implementation. After T005--T019, T024, and
  T029--T033 close, freeze one current source/workload subject and run G0--G3
  once. SIF creation, SIF preflight/replay, remote upload, Slurm allocation,
  and Tiger jobs are refused until G3's real host/CPU MiniNDN M01--M14 manifest
  is `PASS` (42/42 fresh repetitions). A pre-G3 SIF is diagnostic-only. After
  G3, build one final SIF; any later source, contract, or workload edit
  invalidates that candidate and sends the workflow back to implementation plus
  G0.
- A signal exit, core dump, missing case-result record, or unclassified nonzero
  child status from the current subject blocks promotion even when a later
  identical repetition passes. Preserve both attempts, classify the owner, add
  a focused regression, and regenerate the affected triplet and manifest.
- SIF and Tiger are final-candidate validation, not a repair loop. No source,
  dependency, workload, or configuration edit is permitted between the frozen
  seal, exact-SIF replay, and Tiger promotion. If a mismatch appears, preserve
  the evidence, reduce it to the cheapest reproducing G0-G3 case, and begin a
  new candidate only after that lower-cost gate passes again.
- **Execution boundary**: G4 is host-orchestrated. The host runs MiniNDN,
  Mininet, Open vSwitch, NLSR, namespace/routing tools, and the replay driver.
  Inside each host-created namespace, the exact SIF supplies NFD and every
  NDNSF/ONNX Runtime application process through `apptainer exec --cleanenv`.
  MiniNDN/Mininet/OVS/NLSR are forbidden as SIF qualification requirements.
  G4T-G7 run the SIF directly under Slurm and do not run MiniNDN.

### Evidence status and current execution boundary (2026-08-26)

T001--T004, T009--T012, and T021 satisfy their named acceptance gates
(9/34 tasks). The previously passing G0/G1/G2 manifests predate the automatic
Provider-state/no-manual-feedback correction and are historical evidence only;
T020 is deferred until the complete implementation queue closes and can then
run those extended gates once from one current source seal.
A selected G3 host manifest
records 30 historical PASS M01--M10 MiniNDN processes. It predates the
conversation-continuation M11--M14 extension and cannot satisfy the expanded
G3. An additional M09 process reached
its expected terminal and then exited with signal 11; a later same-subject
rerun passed. The historical exit remains a regression input, but it is not an
active campaign-debugging task before the implementation freeze. Earlier
cursor-1, setup-only, and signal-exit attempts remain negative evidence. The candidate-native preflight is
evidence that one SIF can enter G4; it is not a G4 replay or promotion pass.
G4 must record separate host-substrate and SIF-runtime preflights plus the
42-process host-orchestrated replay. G4T--G7 remain unpassed overall.

The job-202864 Qwen3.6-27B CUDA chain used `useCache=false` and has no accepted
decoded transcript. It proves only bounded standalone stage-chain execution,
not the complete stateful Qwen3.6 prefill/decode contract. Older Spec162 jobs,
source overlays, full-context token loops, Provider `READY`, or standalone
stage output cannot be promoted into any gate below.

### Cache-scope evidence rule

Every stateful test must identify its scope as either `request-local` or
`conversation-scoped`. A request-local hit is valid only for a later decode
epoch of the same Request/attempt/generation. Cross-request reuse requires a
committed `ConversationCheckpointV1`, a fresh Request/generation, exact
same-placement role receipts, and delta-prefill over only the appended
canonical suffix. A hit counter around full-prefix execution, a prompt-equality
shortcut, or a lookup by `conversationId` alone fails the test. Manifests must
report the two scopes' hits, promotions, misses/fallbacks, residency movement,
state bytes, and cleanup independently.
The cross-request hit is valid only when each selected Provider restores or
prefetches the promoted conversation-state bundle and the runner consumes it
for suffix-only prefill. A transcript/checkpoint lookup without model-state
readiness, or a full-prefix run hidden behind a hit counter, is a
`FULL_PREFILL_FALLBACK` or a failure—not conversation-scoped reuse.

## Deterministic CPU fixture

Path:

```text
tests/fixtures/spec175/tiny-causal-lm-v1/
```

The committed generator creates canonical content-addressed artifacts, not a
Python fake runner:

- vocabulary size 32;
- hidden size 8;
- four deterministic role blocks that jointly expose attention-KV and bounded
  recurrent/convolution state inputs/outputs, exercising the complete hybrid
  `DecodeStateBundleV1` contract;
- combined one-role ONNX graph and graph-valid 2-role/4-role partitions;
- WordLevel `tokenizer.json` with EOS token ID 2;
- normal prompt token IDs `[11,12,13]` and greedy expected generated IDs
  `[4,5,6,7,8,9,10,2]`;
- early-stop prompt with expected IDs `[14,15,2]`;
- maximum-token prompt with no EOS before 8 tokens;
- manifest containing every graph/initializer/tokenizer/oracle digest.

`tests/fixtures/spec175/build_tiny_causal_onnx.py` must regenerate byte-identical
artifacts under the locked ONNX version or fail with a manifest difference. Test
runners consume the manifest and may not rewrite it.

## G0 - Static and contract gate

Required checks:

1. Spec Kit structure audit, no placeholder or unresolved clarification.
2. TLV collision scan proving all 36 assignments in `0xF661..0xF684` are
   collision-free
   and uniquely assigned afterward.
3. API/wire/default consistency scan across spec, plan, data model, contracts,
   C++ headers, Python options, and CLI manifests.
4. Source-owner map proving the new lifecycle extends current `ServiceUser`,
   `ServiceProvider`, `NDNSFMessages`, `BeginCollaboration`,
   `CommitCollaborationPlan`, `AutomaticPlanningCoordinator`, terminal guard,
   Qwen session, `NativeProviderSession` state owner, epoch coordinator, and
   exact collaboration primitives without a second control plane. The census
   rejects a defined-but-unreferenced decode-state store and formal tests that
   perform manual state feedback.
5. Runtime dependency scan proving deployed entry points do not import or link
   PyTorch/Transformers.
6. Source census proving the streamed symbols/files exist, the registered
   64-token bound and canonical `/LLM/Pipeline/Stage/*` role names agree with
   code, and the user path publishes no per-token service Request.

Current command:

```bash
python3 scripts/spec175_contract_gate.py \
  --feature-dir specs/175-ndnsf-di-streamed-invocation \
  --output results/spec175/g0/qualification-manifest-v1.json
```

Pass: every requirement is mapped, no collision/drift/placeholder exists, and
the manifest is `PASS`.

During development T001 retains one `BLOCKED_EXPECTED` baseline. At the current
checkpoint its only blocker is the unsealed dirty input tree; that is evidence
that the gate fails closed, not a G0 promotion pass. T020 must rerun the same
gate against the sealed subject to `PASS` before T023 can promote.

## Development toolchain closure - required before G1 work

This is an entry preflight, not a promotion gate. Configure the host diagnostic
build with one inseparable NDN-SVS `Experimental` header/library pair:

```bash
./waf configure --with-tests --toolchain-root=/usr/bin \
  --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs \
  --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
./waf build --target=unit-tests -j2
ldd build/unit-tests
```

The configure closure probe must compile and link
`SVSPubSub::subscribeToProducerWithCatchUp`; `ldd` must resolve
`libndn-svs` to that exact build tree and every Boost dependency to 1.71. A new
header with an older installed library is failure. Before Python-native tests
begin in T009, rebuild the host diagnostic `_ndnsf.so` from the same current
framework/toolchain and verify import plus `ldd`. Never copy that host extension
into the SIF.

## G1 - Unit gate

### Native cases

`InvocationStreamMessage`:

- canonical options/event/End/completion round-trip;
- every missing/duplicate/out-of-order/unknown field;
- range, oversize, invalid enum, malformed digest, and noncanonical integer;
- exact Data name build/parse and every component mutation;
- binding/transcript/nonce deterministic test vectors;
- protected-grant length/domain tests, outer/plaintext grant rejection, and
  key-free log/evidence scan;
- End/Response match and mismatch matrix.

`InvocationStreamLifecycle`:

- all valid states and every invalid transition;
- cursor 1 start, no gaps at publisher, duplicate suppression, reorder drain;
- retry 0/1/3 and absolute deadline;
- callback/iterator single-consumer rule;
- publisher/callback/reorder capacity and backpressure timeout;
- cancellation races before admission, after admission, after End, and after
  Response;
- one terminal claim and no callback after terminal;
- Normal and Targeted bindings; unary allocates no stream state.
- service-only Normal API discovery with no Provider vector;
- one shared request ID across streamed `begin_collaboration`, ACK closure,
  `commit_collaboration_plan`, events, End, and terminal Response, with no second
  Request publication;
- Normal Request contains only the event-key commitment; only the final-role
  Selection projection unwraps the matching grant; unselected/nonfinal Providers
  cannot unwrap it.

`DiQwenGenerationSession`:

- one prefill, incremental decode epoch convention, token/event cursor equality;
- EOS and stop success before maximum, exact maximum success, invalid early max;
- incremental Unicode and cross-token stop matching;
- every `DecodeStateIdentityV1` field/component mutated independently and rejected;
- transactional complete decode-state commit/rollback;
- replacement disabled, one replacement enabled, second replacement rejected;
- stale attempt/generation/event/feedback rejection;
- complete final payload/transcript consistency.

`DiNativeProviderSessionState`:

- production `executeRoleAsync` performs prefill commit and later exact state
  lookup/atomic successor commit without caller-managed `*_out -> *_in` wiring;
- the production handler obtains every static identity field from the validated
  artifact/projection/runtime authority and rejects an incomplete template
  before the runner is called; generated fixture digests are test-only;
- only the immediately preceding committed epoch is reusable; every identity,
  component, state/predecessor-epoch, boot-ID, and cache-epoch mutation misses
  before the runner is called;
- prefill derives the logical prefix from canonical prompt-token IDs; each
  decode extends it by one authenticated feedback token; multi-role cases prove
  identical prefix digest/count across roles despite different activation
  bytes, and reject a tampered prefix/count/position commitment;
- a failed execution or downstream publication discards the candidate and
  preserves the predecessor; a successful publication swaps exactly once;
- pinned entries survive capacity pressure, inactive entries use deterministic
  LRU eviction, and oversize/capacity exhaustion fails boundedly;
- cancel, deadline, terminal, attempt fence, Provider restart, and explicit
  failure release the correct entry without affecting concurrent generations;
- decode after a miss never receives synthesized zero state, while prefill is
  the only transition permitted to create initial zero/empty state.
- a matched production-runner cached-versus-full-prefix CPU control has exact
  output parity, one cached-branch prefill, only bounded new input after
  prefill, recorded actual/represented extents, and positive prefix work
  avoided; a hit counter around full-prefix execution fails.

`DiConversationContinuationState`:

- absent conversation options preserve the exact current full-context path;
- a non-conversation terminal releases request-local decode state, and a fresh
  request cannot find it by prompt, generation, or conversation identifier;
- `FULL_CONTEXT` first turn produces no checkpoint until every selected role
  commits the same successor context epoch and logical prefix;
- the first-turn terminal boundary proves whether the last accepted token and
  turn-finalizing template suffix are already represented; any missing suffix
  runs as state-only `CHECKPOINT_FINALIZE` work with zero additional event,
  cursor, sampling result, or callback before promotion;
- `APPEND_DELTA` creates fresh request/generation authority, verifies the parent
  checkpoint and exact prior Provider-role map, and feeds only appended tokens
  plus Provider-local state to delta prefill;
- every checkpoint/receipt identity field, signature, expiry, role-set member,
  prefix, requester/service/security scope, Provider boot/cache epoch, model,
  plan, split, layout, and parent epoch is independently mutated and rejected
  before a runner call;
- multi-role successor commit is atomic; one missing role discards all
  candidates and preserves the parent;
- compare-and-swap admits one of two concurrent children of a parent and returns
  `CONVERSATION_STATE_CONFLICT` for the other;
- independent GPU/host quota emulators exercise deterministic inactive LRU,
  pin protection, single-flight prefetch, cancellation, expiry, and cleanup;
- an exact unavailable state chooses only the sealed full-context fallback or
  explicit failure; it never synthesizes or mixes role state.
- request-local hit/commit counters and conversation promotion/hit counters are
  separate; a request-local hit cannot satisfy a cross-request reuse assertion.

### Python cases

- frozen defaults/ranges and C++ parity;
- bytes and typed codec paths;
- async iterator/result/cancel;
- callback mode and double-consumer rejection;
- Python Provider writer binding, context variant, exception containment, and
  return-without-terminal failure;
- provider `publish_event`/`finish_stream` and duplicate terminal claim;
- no runtime Transformers/PyTorch import.
- Python conversation option validation, opaque checkpoint lifecycle, awaitable
  checkpoint result, full/delta input semantics, fallback opt-in, and absence of
  any public Provider/state-location field.

The G1 Python subject is deliberately scoped to this feature and its shared
stream/DI contract. Historical Spec127--Spec173 frozen-source/result tests are
run only as a diagnostic inventory; they are not allowed to block this feature
promotion unless they fail in a file owned by Spec175 or in a shared API that
the manifest names.

Command:

```bash
./waf configure --with-tests
./waf build -j4
build/unit-tests --log_level=test_suite
python3 scripts/run_spec175_python_gate.py \
  --source-seal results/spec175/g0/source-seal-current.json \
  --output results/spec175/g1/qualification-manifest-v1.json
```

Pass: complete `build/unit-tests` passes without a retry-only waiver; the
bounded Python manifest passes with no unexpected skip; unchanged unary,
Targeted, security, StreamFacade, and Spec 174 focused regressions named in the
manifest pass. A timing-sensitive failure in a complete native run is a real G1
blocker even when the selected case passes in isolation. A full-repository
`pytest tests/python` run remains useful diagnostic evidence but is not a G1
promotion prerequisite for Spec175.

## G2 - CPU in-process integration gate

Boost suites:

```text
Spec175InvocationStream
Spec175DiStreamedGeneration
```

Mandatory cases:

The Provider/role column and tiny-ONNX oracle are part of each case identity.
A generic one-Provider stream test with the same fault shape is a prerequisite,
not that I-case, and MUST NOT be registered under the I-case ID.

| ID | Providers/roles | Fault | Expected |
|---|---|---|---|
| I01 | 1/1 | none | 8 oracle tokens, EOS, one End/Response |
| I02 | 2/2 | none | same oracle; one prefill/plan |
| I03 | 4/4 | none | same oracle; exact activation lineage |
| I04 | 2/2 | event 3 delivered after 4 | callbacks remain 1..8 |
| I05 | 2/2 | event 4 duplicated | one callback for cursor 4 |
| I06 | 2/2 | first fetch of event 5 lost | one retry then exact success |
| I07 | 2/2 | event 5 never available; exercise never-retained, retention-expired, and End-before-gap subscenarios | each subscenario gives explicit `EVENT_TIMEOUT`, no success |
| I08 | 2/2 | cancel after event 3 | exactly 3 callbacks, no Response success |
| I09 | 2/2 + unselected ACK Provider | wrong signer/key/name/binding or unselected/nonfinal key unwrap | reject before reorder insert; only final-role grant decrypts |
| I10 | 2/2 | callback throws at event 3 | contained callback failure/cancel |
| I11 | 2/2 | queue capacity 1, slow consumer | bounded backpressure, no drop |
| I12 | 2/2 + spare | terminal Provider is made unavailable at the real Core publication boundary immediately after event 3; replacement opt-in | one recovery Request uses attempt 2, fresh ACK/plan/Selection/key/epoch, excludes the Provider identified by the validated failure binding, prefills prompt+committed prefix once, and emits only the exact continuation |
| I13 | 2/2 + spare | same deterministic terminal-Provider failure, replacement default | terminal failure, exactly one Request, no re-execution |
| I14 | unary YOLO fixture | none | existing one complete Response, no stream state |
| I15 | 4/4 | permute signed capacity/cache residency across Provider identities | ACK-driven role map changes; exact oracle remains unchanged |
| I16 | 1/1, two turns | exact continuation; turn 1 stops at the fixed maximum with one accepted non-EOS token not yet represented | state-only terminal catch-up emits no event, promotion commits; turn 2 uses fresh Request/generation, delta prefill only, and matches full-transcript oracle |
| I17 | 4/4, two turns | exact continuation with sealed chat-template turn suffix | every role finalizes the same complete turn prefix, promotes atomically, validates one parent checkpoint and same role map, then returns one successor checkpoint and exact oracle |
| I18 | 4/4, three conversations | bounded tier emulation | active state pinned, paused state moved to host tier, single-flight restore, isolated exact outputs |
| I19 | 4/4 | missing/expired/forged/wrong boot/model/plan/role receipt mutations | registered full-prefill fallback or explicit failure; zero incompatible runner calls |
| I20 | 4/4 | concurrent same-parent turns and cancel during prefetch | one successor commits, one conflict; cancellation leaks no state or callback |

For I01-I03 and I15, the production Provider session—not the case body—must
feed committed state into each next role epoch. The manifest records, per role,
one prefill commit, seven decode hits and successor commits for the eight-token
oracle, zero unexpected misses,
zero decode-state dependency objects/bytes, the exact common logical
prefix/count plus state/predecessor/position/cache lineage for every epoch, and
terminal cleanup. The manifest must show that different role-local activation
digests do not replace or authorize the common token-prefix identity. A case that
manually assigns an adapter state output to the next input is a prerequisite
adapter test and cannot be registered under these IDs.

Run each healthy I01-I03, I14-I18 in three fresh processes; run each fault
case I04-I13 and I19-I20 exactly once in a fresh process. I07 contains all three named
subscenarios and records each applied fault. I15 is compared with I03 and fails if Provider-role mapping
is unchanged despite the registered capability permutation. Command:

```bash
python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests \
  --cases I01-I20 \
  --healthy-repeats 3 \
  --seed 1750001 \
  --source-seal results/spec175/g0/source-seal-current.json \
  --output results/spec175/g2/qualification-manifest-v1.json
```

Pass: all exact token/result/state/security oracles match; no leak/high-water
bound exceeds signed options; manifests enumerate all process exits and bind
the G0 source-seal digest plus exact integration-binary SHA-256.

## G3 - Host/CPU real MiniNDN gate

Run the four-Provider M01-M14 matrix against the current native/Python build
and the checked-in tiny ONNX fixture before building any SIF. The topology,
faults, packet lineage, and disabled-admission contract below are the primary
low-cost debugging subject.

The deterministic host baseline runs with
`NDNSF_SELECTION_TARGETED_PREFETCH=0`; this keeps the authenticated SVS
Selection publication as the required path. Targeted prefetch is an optional
transport experiment and cannot be needed for a G3 pass or used to mask a
Selection race.

Pass: 42/42 fresh case repetitions pass, every same-subject attempt is retained
and no signal/native exit remains unclassified, process-tree cleanup succeeds, exact
oracle and transcript closure hold, all capacities remain bounded, and the
manifest is complete. A failure returns to the owning implementation or
MiniNDN test; it does not start a SIF rebuild.

## G4 - Final local SIF build, native preflight, and replay

After G0-G3 pass, build one SIF through the current local Apptainer entry point and a frozen
definition/source-seal/host-gate tuple. No Docker build, remote materialization, host-built Python
extension, or host site-packages may enter the candidate.

The exact framework, Python SDK, adapters, and workload used by G0-G4 must be
embedded in the candidate. Runtime source/check-out overlays and replacement
Python module bind mounts are forbidden for G4-G7. External model artifacts are
allowed only through the immutable content-addressed model manifest.

The runtime preflight, inside the exact SIF, must verify:

- SHA-256 source seal and SIF hash;
- Python 3.10 executable, SOABI, `EXT_SUFFIX`, and real `import ndnsf._ndnsf`;
- native extension/library hashes, RPATH, and complete `ldd` closure;
- Boost 1.71, ndn-cxx, NDN-SVS `Experimental`, NFD, NDNSF, and pybind11 ABI;
- ONNX Runtime CPU and CUDA provider inventory and the exact libraries;
- `tokenizers` available; PyTorch and Transformers absent from deployed runtime;
- Spec 175 API symbols and `--help` for the exact workload entry point;
- artifact/model/tokenizer/workload paths resolved from the verified bundle cwd;
- writable scratch/output paths, no secret/key leakage, and bounded free-space
  check inherited from the current native SIF preflight.

A separate host-substrate preflight must verify MiniNDN, Mininet, Open vSwitch,
NLSR, `mnexec`, `ip`, topology input, namespace privileges, the tracked replay
driver, and `/opt/apptainer/1.5.3/bin/apptainer` version 1.5.3. These host tools
must not be copied into or required from the SIF. The G4 manifest records both
preflight results independently.

The executable host-layer gate is:

```bash
packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --repository-root "$PWD" \
  --topology-file "$PWD/Experiments/Topology/spec175-host-gate.conf" \
  --replay-driver "$PWD/packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py" \
  --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --expected-apptainer 1.5.3 \
  --output results/spec175/g4/host-substrate-preflight.json
```

The exact-SIF gate must not repeat these host checks. A runtime result that
requires `mn`, `mnexec`, `ovs-*`, `nlsr`, Mininet, or MiniNDN is invalid and
must be classified as `WRONG_LAYER_PREFLIGHT`.

The builder also consumes the immutable host-gate manifest and must refuse a
build when that manifest is absent, failed, incomplete, or bound to a different
source/workload subject.

Planned commands:

```bash
packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --definition "$SPEC175_DEFINITION" --sif "$SPEC175_SIF" \
  --record "$SPEC175_BUILD_RECORD" --source-seal "$SPEC175_SOURCE_SEAL" \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --strict-host-source-seal --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --expected-apptainer 1.5.3

packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight \
  --sif "$SPEC175_SIF" --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --source-seal "$SPEC175_SOURCE_SEAL" \
  --workload packaging/ndnsf-di-container/jobs/spec175/workload.json \
  --output results/spec175/g4/sif-runtime-preflight.json
```

Pass of this section means the candidate is `READY_FOR_G4_REPLAY`; there is one
and only one SIF digest. `PROMOTABLE` is emitted only after the replay below.

Replay the same host/CPU M01-M14 manifest with the host creating the MiniNDN
topology and using the exact SIF as the command provider for each node's NFD and
NDNSF/ORT processes. This is a packaging/reproducibility check, not the primary
debugging loop. Any mismatch is first classified as host-substrate,
container-launch/mount/environment, or SIF-runtime failure before a new
candidate is considered.

## G4 replay subject: host MiniNDN with exact-SIF NFD/NDNSF processes

Topology:

```text
controller c ----+
repository repo --+
user u -----------+-- router a
provider p0 ------+
provider p1 ------+
provider p2 ------+
provider p3 ------+
```

Every access link is 100 Mbit/s, 10 ms one-way delay, queue 1000 packets, and
0% baseline loss. MiniNDN/Mininet/OVS and host NLSR create and manage this
emulated substrate. Every named node runs its own NFD from the exact SIF;
Controller, repository, User, and Providers are separate SIF-contained OS
processes started inside their assigned host namespaces. The controller
distributes real permissions/ABE credentials. The repository serves the
hash-bound tiny ONNX bundle mounted read-only. Admission control is disabled
and explicitly recorded; a host NFD, host `_ndnsf.so`, host NDNSF process,
Python fake transport, or in-SIF MiniNDN process is invalid.

Mandatory cases, each in three clean MiniNDN processes. M11--M14 use the same
topology and tiny ONNX bundle and add no SIF or GPU dependency:

| ID | Roles | Fault | Expected |
|---|---:|---|---|
| M01 | streamed 4P | healthy baseline | 8 exact tokens, one End/Response, four-role lineage |
| M02 | streamed 4P | event reorder | ordered callbacks and exact final Response |
| M03 | streamed 4P | duplicate event | duplicate suppressed; exact final Response |
| M04 | streamed 4P | one event publication lost | bounded exact retry and success |
| M05 | streamed 4P | permanent event gap | explicit bounded failure; no partial success |
| M06 | streamed 4P | callback exception | contained callback failure; terminal state recorded |
| M07 | streamed 4P | cancel after event 3 | no later callback or success Response |
| M08 | streamed 4P | request deadline | bounded deadline failure; no leaked process |
| M09 | streamed 4P | Provider failure | failure is surfaced and process is cleaned up |
| M10 | streamed 4P | signed capacity/cache permutation | ACK-driven plan mapping changes from M01; exact result and lineage remain valid |
| M11 | streamed 4P, two turns | exact continuation with a deterministic unrepresented terminal suffix | request-local state is finalized/promoted after turn 1 with zero extra event; fresh turn-2 Request/generation uses the same role map and only the appended canonical suffix, with full-transcript oracle parity and one successor checkpoint |
| M12 | streamed 4P, three conversations | CPU tier emulation | isolated active/paused state, deterministic host-tier restore, bounded quotas, zero state bytes on NDN edges |
| M13 | streamed 4P, two turns | missing role, expiry, Provider restart, wrong checkpoint/plan/model mutations | explicit full-prefill fallback when sealed, otherwise exact continuation failure and zero incompatible runner calls |
| M14 | streamed 4P, two competing turns | same parent plus cancellation during one prefetch | one successor commit, one conflict/cancel result, parent remains valid, leak-free teardown |

The harness records Request -> ACK closure -> plan -> Selection -> artifact
fetch/assembly -> prefill -> every activation -> token feedback -> external
event -> End -> final Response, including exact Interest/Data names.

Normative command boundary after T023 implements the tracked driver:

```bash
sudo -E env \
  SPEC175_RUN_REAL_MININDN=1 \
  SPEC175_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer \
  SPEC175_RUNTIME_SIF="$(jq -er .sif.path "$SPEC175_CANDIDATE_MANIFEST")" \
  SPEC175_CANDIDATE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
  python3 packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py \
  --host-gate-manifest "$SPEC175_HOST_GATE_MANIFEST" \
  --source-seal "$SPEC175_SOURCE_SEAL" \
  --sif "$SPEC175_SIF" --apptainer /opt/apptainer/1.5.3/bin/apptainer \
  --sif-sha256 "$(sha256sum "$SPEC175_SIF" | awk '{print "sha256:"$1}')" \
  --output results/spec175/g4/qualification-manifest-v1.json
```

Pass: 42/42 fresh replay repetitions pass, process-tree cleanup succeeds, exact
oracle and transcript closure hold, all capacities remain bounded, every NFD and
application command is recorded as an invocation of the exact SIF, no host
runtime substitution occurs, and evidence binds the G4 SIF, host harness,
topology, fixture, Apptainer, and G3 manifest digests.

## G4T - Tiger current-SIF deployment control

G4T reuses the proven Spec170 D0/r23 operational shape before any 27B artifact
is staged. A machine-readable delta manifest compares the rendered Spec175
launcher with D0 Job 189483 and, where relevant, r23 D2b/D2h. It records the
exact SIF/bundle hashes, Apptainer 1.5.3 path/version, Python ABI, explicit
bundle `cwd`, isolated HOME/PIB/TPM, NFD socket, Provider count, timeouts,
node/GPU layout, runtime provider, scratch paths, and child-exit propagation.
Every intentional difference is named; an unlisted difference fails before
`sbatch`.

Run one bounded CPU/no-GPU job with the exact current SIF: one Controller, one
User, four Providers, real NFD, Request, four ACKs, provider-specific Selection,
and one final Response. Tiger verifies and stages the SIF but does not run
MiniNDN/NLSR, build/materialize an image, or overlay source. Every child must
exit zero and Slurm must report `COMPLETED 0:0`. G4T is deployment evidence only
and cannot satisfy G5--G7. Failure blocks model staging and must be reduced to
one delta from the historical control.

## G5 - Tiger Qwen3.6-27B per-stage CUDA and cache-readiness gate

G5 uses the exact pinned Qwen3.6-27B stage artifacts and candidate that G6 will
use. It is not a small-model generation gate and does not claim an NDNSF-DI
streamed response.

The input manifest must describe adapter-certified stateful prefill and decode
graphs/I/O for the complete Qwen3.6 `DecodeStateBundleV1`. The existing
full-context-only reference chain is an oracle prerequisite, not the G5 subject.
The manifest must also seal `decodeMode=single-token-autoregressive`,
`modality=text-only`, `mtpEnabled=false`, `thinkingMode=disabled`, and the exact
chat-template digest, enumerate the language-model-only graph closure, and prove
the source model's vision encoder/projector and MTP/speculative heads are not
loaded by any stage.
The G5 oracle must be generated by the same stateful CUDA ORT stage chain used
by the candidate, carry the exact tokenizer digest, contain in-vocabulary token
IDs and a nonempty decoded transcript, and prove incremental decode with the
complete state bundle. A `useCache=false` campaign or a manifest with an empty
decoded transcript is diagnostic only.

For each of Stage0 `[0,21)`, Stage1 `[21,42)`, and Stage2 `[42,64)`:

1. stage the immutable artifact once into a content-addressed node-local cache;
2. verify artifact, adapter, graph, initializer, tokenizer, and cache digests;
3. load the stage with ONNX Runtime `CUDAExecutionProvider` from the exact SIF,
   assigning Stage0, Stage1, and Stage2 to three distinct allocated CUDA
   devices; a one-GPU all-stage load is invalid;
4. execute registered prefill and one incremental decode transition, preserve
   every state component, and match the reference output;
5. retain the complete state in CUDA-resident buffers through persistent I/O
   binding, record logical state bytes and host-transfer bytes/time, and prove
   zero complete-state device-to-host-to-device round trips after prefill;
   The graph contract MUST pair each state input and successor output with the
   same tensor element type; a mismatch is an artifact failure before execution.
6. run a bounded eight-token paired cache-effectiveness control using the same
   stage, inputs, outputs, precision, and GPU: one excluded warmup followed by
   three alternating-order cached-versus-full-prefix diagnostic pairs;
   require exact output parity, one-token cached decode inputs, monotonically
   advancing state epoch/length, and lower median cached model-compute time;
7. record CUDA provider/device, memory, load/execute timing, state
   hit/miss/commit/eviction/cleanup counters, and zero CPU model-compute
   fallback; and
8. prove the later G6 launcher resolves the same cache paths without copying or
   hashing the multi-gigabyte stage inside the measured generation interval.

Pass: all three stage readiness records pass on the allocated GPU class and the
combined manifest binds the G4 SIF, G4 replay proof, source/workload, model, cache, and
reference identities. It retains every paired-control unit and its ordering;
an equal/slower cached median is a readiness failure, not a discarded outlier.
A Qwen3-0.6B CUDA run is an optional diagnostic only and
cannot satisfy G5.

## G6 - Tiger three-Provider Qwen3.6-27B functional gate

Model: `Qwen/Qwen3.6-27B`, immutable revision
`6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`.

The source repository is multimodal and MTP-capable, but the registered subject
is its language-model-only text subgraph with MTP/speculative decoding disabled.
Every measured decode epoch samples exactly one token, publishes exactly one
feedback object, and admits exactly one external token event. A run that loads
the vision encoder/projector or accepts speculative/MTP tokens is a different
subject and fails G6.

The immutable workload file is
`packaging/ndnsf-di-container/jobs/spec175/workload.json`. It must be created and
validated before the G4 SIF is sealed and then remain byte-identical through
G7. It contains exactly these two UTF-8 user messages:

```text
P1: Write exactly 64 numbered words about content-addressed model artifacts. Do not stop before word 64.
P2: Generate a 64-item comma-separated sequence alternating the words network and model. Do not add an introduction or stop early.
```

The file also seals prompt digests, derived input-token IDs/counts, tokenizer
and chat-template digests, `thinkingMode=disabled`, Greedy,
`maxGeneratedTokens=64`, `maxEvents=65`, and
`requestDeadlineMs=120000`. G5 generates the exact ONNX CUDA output oracle for
these already-frozen inputs; it may not replace a prompt after observing EOS or
performance. Evidence names only P1/P2 plus hashes and sizes.

The frozen layer split is retained. The following mapping is the expected result
of the pinned signed ACK capability/residency manifest, not a caller-supplied or
offline Provider binding:

```text
Stage0 layers [0,21)  -> Provider P0
Stage1 layers [21,42) -> Provider P1
Stage2 layers [42,64) -> Provider P2
```

Each Provider uses one allocated GPU and one complete role. The plan is built
from the ACK capability snapshot and shared through Selection; no offline
Provider binding is accepted. Offline work only exports canonical ONNX
graph/weights/adapter metadata.

Compatibility precondition: this model revision is hybrid. Its
`linear_attention` layers carry convolution/recurrent inference state, while
its `full_attention` layers carry KV state. The adapter must expose both through
the exact `DecodeStateBundleV1` contract. Healthy decode epochs consume only the
new token plus that compatible provider-local bundle. `useCache=false` or
complete prompt/prefix recomputation per token may be retained as an explicitly
labelled oracle/diagnostic, but it fails G6 and cannot feed G7 performance.

The functional workload uses those two fixed prompt digests, Greedy sampling,
`maxGeneratedTokens=64`, `maxEvents=65`, one excluded cold preparation, and three fresh warm
Provider/User processes per prompt. Pass requires 6/6 exact successful measured
invocations, one plan/prefill per generation, exact activation/feedback/event
lineage, one Provider-owned incremental decode-state hit/transition/commit per
role and non-prefill epoch, zero unexpected state miss/recompute, zero
decode-state bytes on NDN edges, zero complete-state host round trips, bounded
terminal cleanup, zero CPU model-compute fallback, all three GPUs used by their
roles, no runtime PyTorch/Transformers import, and no runtime source overlay.

## G6C - Tiger conversation-residency functional control

G6C runs only after the same candidate passes G6 and before G7. It is a bounded
functional control, not a throughput campaign. The frozen continuation workload
contains one conversation with two turns and a second paused conversation used
to create deterministic state-tier pressure. Both use the same pinned model,
tokenizer, chat template, three-role placement policy, and Greedy sampling as
G6, with separately registered input digests and `maxGeneratedTokens=8` so this
control does not silently change or enter the G7 sample.

The first turn performs full prefill and commits one aggregate checkpoint. The
state manager then moves that conversation's state on all three roles from GPU
to the bounded host pool while keeping model weights resident. The second turn
submits only its appended input and parent checkpoint. Each role records
`HOST_RESIDENT -> PREFETCHING -> GPU_RESIDENT`, reaches the all-role readiness
barrier, performs delta prefill, and produces the same tokens/text as the
full-transcript CUDA oracle.

Pass requires one exact successor checkpoint, fresh request/generation
identities, the unchanged one-to-one role map, exact role receipts and context
epochs, positive and matching transfer bytes, finite prefetch latency, no state
tensor bytes on NDN edges, no model-weight transfer, no CPU model-compute
fallback, no incompatible runner call, and bounded cleanup. It additionally
runs one unavailable-role mutation with fallback disabled and proves explicit
`CONVERSATION_STATE_UNAVAILABLE`, then repeats with the sealed full-context
fallback enabled and proves one recorded full prefill and exact output. G6C
makes no latency or memory-saving claim; it qualifies only the real CUDA/host
residency mechanism.

## G7 - Performance qualification

G7 reuses the passing G6 candidate and workload without changes. For each warm
invocation report:

- TTFT;
- every inter-token interval and p50/p95/p99;
- steady-state tokens/s, excluding first token and terminal drain;
- total latency and exact success;
- per-role ORT, activation fetch, sampling, feedback, event publication/fetch,
  and queue wait time;
- retransmissions, gaps, duplicate suppression, complete decode-state
  hit/miss/recompute/commit/eviction/cleanup and logical resident bytes;
- per-role state residence plus complete-state host-transfer bytes/time;
- GPU utilization/memory, CPU, RSS, network bytes, CPU model-compute fallback,
  and bounded CPU shape-control counts.

Verdict:

```text
PERFORMANCE_PASS iff
  all G6 correctness checks pass AND
  all G5 paired cache-effectiveness controls pass AND
  median warm steady-state tokens/s >= 20.0 AND
  p95 warm inter-token interval <= 75 ms

otherwise, if correctness passes:
  FUNCTIONAL_PASS_PERFORMANCE_MISS
```

No valid warm run is discarded. Summaries report count, failure count, min,
median, mean, p95, maximum, and per-process values. Performance miss evidence
must attribute the largest measured component; it must not trigger unregistered
parameter tuning inside the same campaign.

## Tiger submission command contract

Only the following top-level interface is allowed:

```bash
CLOSURE_MANIFEST="$SPEC175_CANDIDATE_MANIFEST" \
SIF="$SPEC175_SIF" SIF_SHA256="$SPEC175_SIF_SHA256" \
WORKLOAD="$SPEC175_WORKLOAD" REMOTE_SIF="$SPEC175_REMOTE_SIF" \
REMOTE_SIF_SHA256="$SPEC175_SIF_SHA256" \
PRE_TIGER_CHECKLIST="$SPEC175_PRE_TIGER_CHECKLIST" \
PRE_TIGER_CHECKLIST_VALIDATION="$SPEC175_PRE_TIGER_CHECKLIST_VALIDATION" \
packaging/ndnsf-di-container/jobs/spec175/submit.sh \
  control|stage-readiness|multi-provider|performance
```

Before any SSH, upload, remote mutation, or `sbatch`, the script must invoke
the tracked repository validator
`packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist` on
`PRE_TIGER_CHECKLIST`, write `PRE_TIGER_CHECKLIST_VALIDATION`, and require its
candidate ID, mapped checklist gate, exact SIF digest, required rows, and
evidence hashes to match the submission. The validator implements
`ndnsf-itiger-pre-submit-checklist-v1` inside the repository; the similarly
named operator-side checker is only a reference and must not become a runtime
dependency. A checklist PASS proves completeness and binding, not the semantic
correctness of the underlying repository preflights.

The frozen required rows for `control` are:

```text
release-identity
local-sif-route
cluster-substrate
target-apptainer-parity
container-abi-provenance
complete-target-link-closure
exact-sif-library-entrypoint
bundle-cwd-artifact-mount
isolated-home-pib-bootstrap
lower-gates-native-exits
wrapper-config-child-status
resource-envelope
promotion-hash-config-delta
credential-secret-scan
```

`stage-readiness`, `multi-provider`/`functional`, `conversation-residency`, and
`performance` additionally
require:

```text
current-sif-tiger-control
onnx-model-runtime-compatibility
cuda-no-fallback
routes-stage-dataflow
conversation-checkpoint-state-tier
```

Gate prerequisites are cumulative and candidate-bound: `control` requires G4;
`stage-readiness` requires the passing current-SIF Tiger control;
`multi-provider` requires that same candidate's passing G5 stateful-stage and
cache-effectiveness manifest; `conversation-residency` requires the same
candidate's passing G6 functional manifest; `performance` requires G5, G6, and
the same candidate's passing G6C conversation-residency manifest. A manifest from another candidate,
SIF, workload, model, or source seal is missing evidence, not a reusable pass.

Unknown rows fail closed so a misspelled requirement cannot appear satisfied.
Each row requires `status=PASS` and at least one existing, nonempty evidence
file whose declared SHA-256 matches its bytes.

The script must then run login preflight, bounded compute-node preflight,
historical-control delta validation, SIF/config/model hash verification,
rendered Slurm validation, explicit bundle cwd, and
remote free-space check before `sbatch`. It uploads only missing content-addressed
artifacts and never rebuilds or substitutes the SIF. Any preflight mismatch stops
before network mutation or GPU allocation. There is no checklist override flag.
The `control` gate requires no model manifest or GPU; model/cache variables
become mandatory only for `stage-readiness`, `multi-provider`, and
`conversation-residency`, and `performance`. Do not document a flag-style `--gate` interface while the
tracked script uses one positional gate plus environment-bound immutable inputs.
