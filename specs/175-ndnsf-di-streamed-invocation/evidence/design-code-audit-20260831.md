# Spec175 design-to-code audit — 2026-08-31

## Verdict

**BLOCK — do not reseal G0, build another SIF, or submit another Tiger job.**

The current source proves the generic streamed Core, ACK-driven V3 planning,
one-to-one V3 assignment, NDN activation/token-feedback transport, bounded
stream delivery, and CPU/tiny-ONNX lifecycle. It does **not** yet implement the
complete NDNSF-DI design that Spec175 claims for the production Native Provider
and CUDA/conversation paths. The existing host MiniNDN and small-model GPU
passes remain useful regression evidence, but they cannot close the gaps below.

## Design baseline used for this audit

1. One Provider owns one complete ordinary pipeline role, and one Provider is
   not reused for another role in the same plan.
2. `PreSplitFirstStrategy` is the ordinary default. Cross-Provider tensor
   parallelism remains the separate Spec174 TensorGroup/rank-role contract and
   is not silently admitted by Spec175 streaming.
3. Request and ACK collection precede placement. The User seals only the
   canonical model identity, role assembly recipe, and Provider assignment;
   each selected Provider fetches canonical ONNX content and assembles/loads its
   own role after Selection.
4. The deployed runtime uses ONNX Runtime plus a standalone tokenizer, not
   PyTorch/Transformers.
5. One LLM Request performs prefill once, then repeated decode epochs. The
   terminal role samples, incrementally detokenizes, sends a token-feedback Data
   object to the first role, emits an application event, and eventually returns
   a decoded final result.
6. Full-attention KV state and linear-attention recurrent/convolution state stay
   Provider-local. The healthy CUDA path does not serialize the complete state
   through a host `TensorBundle` on every token.
7. Conversation state is promoted only after all roles commit. `GPU_RESIDENT`,
   `HOST_RESIDENT`, and prefetch transitions describe actual allocation and
   transfer, not accounting labels.

## Findings

### F01 — Critical — production Native Provider bypasses post-Selection assembly

The V3 User side correctly seals a canonical root and certified
`RoleAssemblySpec`, and the Python library contains `assemble_onnx_role_v3()`.
However, the production native executable materializes complete per-role model
files before Provider startup and unconditionally enables
`allowPreassembledV3Compatibility`:

- `examples/DI_NativeProviderExecutable.cpp:803-825`
- `examples/DI_NativeProviderExecutable.cpp:1439-1455`
- `NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.cpp:282-307`

No production setup assigns `NativeProviderHandlerConfig::runnerPreparationFactory`;
its current non-test consumers are only the handler seam. The native
materializer fetches a ready `model` entry and changes `spec.path`; it does not
fetch the canonical root referenced by Selection, validate the sealed recipe,
assemble the selected layer range, sign/cache the assembled role, and load it
through ORT. The correct Python assembly implementation at
`NDNSF-DistributedInference/ndnsf_distributed_inference/artifact_deployment.py:175-276`
is therefore not on the native workload path.

**Required correction:** wire a production `runnerPreparationFactory` that
performs bounded post-Selection canonical fetch, recipe verification,
Provider-local assembly, Provider-signed cache activation, ORT load/readiness,
and exact evidence. Formal Spec175 runs must reject the preassembled
compatibility flag; preserve it only for explicitly named legacy fixtures.

### F02 — High — Spec175 streaming does not fail closed at the ordinary V3 boundary

`request_streaming()` accepts an arbitrary strategy. `request()` dispatches to
V3 only when the strategy advertises `DI_PLACEMENT_V3` and otherwise continues
through the legacy V2 path
(`app_sdk/placement.py:1915-1989`, `:2305-2349`). The V2 placement scorer
accounts accumulated memory but intentionally permits the same Provider to own
several roles (`planner/presplit_first.py:173-287`). That violates Spec175's
ordinary one-Provider/one-role invariant.

The V3 streamed path also accepts candidate rank sets and serializes
`hybrid_plan.redistributions` (`app_sdk/placement.py:2778-2820`), even though
Spec175 explicitly leaves streamed TensorGroup/rank-role generation outside its
scope. The underlying Spec174 feature should remain available, but it must not
enter the Spec175 public streaming API implicitly.

**Required correction:** before the first Request, require the Spec175 streaming
profile to be ordinary `DI_PLACEMENT_V3`; after candidate selection and again
before plan commit, reject a hybrid plan, `tensor_degree != 1`, duplicate
Provider ownership, incomplete roles, or a V2 strategy. Add negative tests that
prove the separate Spec174 TensorGroup API remains unchanged.

### F03 — Medium — the registered workload names a compatibility strategy as default

The Qwen V3 workload selects `LayerReuseFirstStrategy`
(`examples/python/NDNSF-DistributedInference/llm_pipeline/user.py:765-781`),
while that class documents itself as a compatibility name that is not the
normal application default
(`planner/layer_reuse_first.py:1-28`). It currently inherits the corrected V3
one-to-one logic, so this is not a placement-correctness failure today; it is a
configuration and evidence-identity drift that could hide a later semantic
difference.

**Required correction:** use `PreSplitFirstStrategy` in the registered Spec175
workload and retain `LayerReuseFirstStrategy` only for explicit compatibility
tests.

### F04 — Critical — native text streaming was previously token-ID-only

At the original audit point the native coordinator always wrote an empty
`textDelta` and its final payload contained only `tokenIds` and `finishHint`
(`NativeEpochCoordinator.cpp:510-550`). The T039 repair slice now accepts a
Provider-injected standalone decoder, checks that decoded text preserves the
committed prefix, emits the decoded delta, matches stop strings over the
accumulated text, and includes final text plus a lower-case finish reason. The
slice is not yet a closed finding: no current four-Provider process result has
proved Unicode, split-stop, EOS, and max-token parity through the production
Selection path, and no final convergence audit has passed.

**Required correction:** the selected terminal Provider must load the sealed
standalone tokenizer/chat-template identity, incrementally decode each accepted
token, match stop strings across token boundaries, emit the real `textDelta`,
and return both the exact token transcript and decoded final text. Tests must
include a multi-byte Unicode boundary, an empty legitimate delta, a cross-token
stop match, EOS, stop, and max-token completion through the real native
multi-Provider path.

### F05 — High — native sampling previously ignored the sealed policy

At the original audit point the native coordinator called `greedyToken()`
unconditionally (`NativeEpochCoordinator.cpp:483-508`, `:811-829`). The T039
repair slice now carries the complete authenticated sampling parameters and
implements Greedy plus deterministic seeded Top-K/Top-P with repetition
penalty. The slice remains open until native/Python parity vectors and a
production multi-Provider process case prove that the sealed mode is executed,
not merely represented by a digest.

**Required correction:** carry the complete authenticated sampling parameters
to the terminal role and implement the same deterministic sampler in the native
production path, or reject every mode except Greedy before Request publication.
The Spec175 design keeps both registered modes, so the remediation plan adopts
the former and requires native/Python parity vectors.

### F06 — Critical — multi-Provider CUDA decode still materializes full state on the host per token

The ONNX runner has a correct device-retention branch only when
`RoleExecutionContext.streamingStateExecution` is true. Otherwise each state
output is copied from its retained CUDA `Ort::Value` into a host buffer
(`OnnxRuntimeModelRunner.cpp:1120-1145`). The direct single-runner
`runStreamedImpl()` sets that flag (`:1308-1321`), but the actual
`NativeEpochCoordinator` submits one ordinary `executeRoleAsync()` call per
epoch (`NativeEpochCoordinator.cpp:779-812`), and `ProviderRoleWorker` never
sets `streamingStateExecution` in that coordinator-owned context
(`ProviderRoleWorker.cpp:804-829`). The Provider decode-state transaction then
stores the host `TensorBundle`.

This means the direct adapter test and the distributed production loop have
different state movement. The latter violates the stated zero complete-state
host-round-trip requirement and can erase the intended decode performance.

**Required correction:** make the coordinator-owned multi-role path commit an
opaque runner/device state transaction rather than a serialized state
`TensorBundle`. Every role, including nonterminal roles without an event sink,
must retain and rebind exact predecessor device allocations. CPU remains the
explicit host-tensor control. Add transfer counters from the actual copy seam;
a counter-only claim is insufficient.

**Focused repair checkpoint (2026-08-31):** `RoleSpec::streamingStateExecution`
now propagates through `ProviderRoleWorker` into the native runner. The CUDA
ONNX adapter retains state outputs in its ORT device map, returns a small
opaque Provider-local handle for the decode-state transaction, and exposes
session-state cleanup through `NativeModelRunner::releaseSessionState()`. The
focused coordinator regression passes this propagation path. The finding
remains open because no real multi-Provider CUDA run has yet measured D2H/H2D
bytes or proven the no-complete-state-host-round-trip invariant.

### F07 — Critical — conversation GPU/host residency is accounting-only

`ConversationStateStore` owns a `TensorBundle`. `stagePromotion()` labels those
host bytes `GPU_RESIDENT` and increments GPU byte accounting
(`NativeProviderRuntime.cpp:605-646`). `pauseToHost()` only moves accounting and
changes the enum (`:870-893`); `prefetchToGpu()` asynchronously changes the enum
and counters without allocating/copying a device buffer (`:896-960`). Thus the
current MiniNDN M12 evidence proves lifecycle messages, isolation, and bounded
state bookkeeping, not physical GPU-to-host-to-GPU movement.

**Required correction:** reopen T031. Conversation entries must own either an
actual ORT/CUDA device allocation or a host allocation, perform real D2H/H2D
copy at the transition seam, transfer ownership transactionally from
request-local state, release/zeroize the old allocation, and report measured
bytes/latency from those operations. G6C remains blocked until this is observed
with the real Qwen role state.

### F08 — Medium — most DI runtime diagnostics bypass ndn-cxx log filtering

`RuntimeTiming.cpp` uses a named `NDN_LOG` component, but the epoch coordinator,
Provider handler, and worker still emit many `std::cout` records. Those records
cannot be controlled by `NDN_LOG` severity/component filters and have already
caused interleaved evidence problems.

**Required correction:** route diagnostic/timeline/evidence records through
named `NDN_LOG` components and severity levels; reserve stdout for a small
atomic operator summary. Preserve the existing privacy rule: no prompt, decoded
answer, state tensor, token payload, or key bytes in logs.

### F09 — High — current gates can pass while F01/F04/F06/F07 remain false

The tiny-ONNX native integration checks token IDs and final token arrays, not
nonempty/Unicode text deltas. G3/G4 use preassembled startup artifacts. CPU
conversation cases can validate enum transitions without a device allocation.
The small-model Tiger diagnostic proves CUDA ORT execution and NDN role flow,
but it is neither stateful nor autoregressive. Therefore the current
unit/integration/MiniNDN/SIF results are valid only for the behavior they
observe; they do not establish production assembly, native text generation,
multi-role device-resident KV state, or real conversation tiering.
The existing gate regression also hard-codes the pre-audit inventory of 80
functional requirements and 19 success criteria
(`tests/python/test_spec175_contract_gate.py:198-199`); after FR-079/FR-080 and
SC-020/SC-021 it must require 82 and 21 and prove their mappings.

**Required correction:** add fail-first production-path tests for F01-F08 and
make them prerequisites to any new G0 seal. G3 must start Providers without
preassembled role models, require assembly after Selection, require real text
events, and expose state-copy counters. CUDA physical-residency claims remain
G5/G6/G6C evidence and cannot be closed by a CPU emulation.

## Confirmed design-aligned code

- `APPClient.request_streaming()` is a real public model/task-first API and
  accepts no Provider list.
- Default replacement remains disabled; Targeted replacement rejects.
- The normal V3 `PreSplitFirstStrategy` uses a distinct-Provider set and creates
  a one-to-one mapping.
- ACK closure precedes graph candidate enumeration and placement.
- The final role publishes token feedback for the next epoch, and upstream
  roles fetch it through the declared NDN dependency path.
- Request-local and conversation identities are separate and bind request,
  attempt, plan, generation, role, Provider boot/cache epoch, and prefix.
- State tensors are removed from NDN dependency outputs.
- Deployed SIF preflight excludes PyTorch/Transformers and verifies ORT/CUDA
  providers. This packaging property remains valid but cannot substitute for
  the missing runtime semantics above.

## Readiness scorecard

| Area | Status | Meaning |
|---|---|---|
| Generic streamed Core | PASS | Ordered event/End/Response, bounded retry/backpressure/security are implemented and exercised. |
| ACK-driven ordinary V3 placement | PARTIAL | Default V3 is one-to-one, but streamed API does not reject V2/hybrid scope escapes. |
| Provider-owned post-Selection assembly | MISSING | Python primitive exists; production native Provider still uses preassembled startup artifacts. |
| Native autoregressive control loop | PARTIAL | Prefill/decode/token-feedback loop exists, but sampling/text output are incomplete. |
| Request-local CPU state | PASS | Exact identity and transactional host `TensorBundle` behavior are covered. |
| Request-local CUDA state | PARTIAL | Direct runner retains CUDA values; multi-Provider coordinator still copies full state to host each epoch. |
| Conversation transaction | PARTIAL | Cross-request identity/commit protocol exists; physical GPU/host tiering does not. |
| Host MiniNDN | PASS at lower scope | Proves NDN/process/protocol behavior with tiny ONNX, not the missing CUDA/assembly/text semantics. |
| SIF/Tiger promotion | BLOCKED | Existing candidates are historical after the required source/spec changes. |

## Required implementation order

1. Enforce the ordinary V3 placement boundary and registered default.
2. Wire production post-Selection Provider assembly and remove compatibility
   from the formal path.
3. Wire native sampler, incremental tokenizer, text events, and final text.
4. Replace per-token host state serialization with device-owned transactions.
5. Implement real conversation GPU/host transfers and reopen T031 until proven.
6. Normalize logging and add the production-path regressions.
7. Run focused unit/integration tests, then one fresh G0-G3 sequence.
8. Only after all implementation tasks close, build one new SIF and proceed to
   G4, G5, G6, G6C, and G7.

## Implementation checkpoint — ordinary V3 boundary (2026-08-31)

The first repair slice for F02/F03 is implemented, but the overall BLOCK
verdict remains unchanged. The public automatic streamed path rejects
non-`DI_PLACEMENT_V3` strategies before request submission. After a V3
strategy proposes a candidate, the coordinator rejects hybrid/TensorGroup
plans, non-one tensor degrees, non-zero ranks, rank-role kinds, incomplete role
sets, and non-one-to-one Provider assignments before canonical publication or
Selection. The registered Qwen workload now names `PreSplitFirstStrategy`
explicitly; `LayerReuseFirstStrategy` remains an explicit compatibility class.

Focused evidence:

```text
pytest -q tests/python/test_spec175_v3_boundary.py \
  tests/python/test_spec170_default_application_path.py \
  tests/python/test_streamed_invocation_api.py
40 passed
```

The prior Spec170 hybrid streamed-path assertion was converted into a
fail-closed boundary regression, and the generic `request(...,
generation_mode="TOKEN_STREAMING")` entry point now has its own V2
pre-publication negative. Separate Spec174 hybrid/TensorGroup tests remain
unchanged. T037 is still open until its complete named process-integration
negative matrix and the post-repair convergence audit are recorded; F01 and
F04--F08 remain controlling gaps.

The first T038 interface slice is also present: the native Selection projection
now receives the authenticated canonical artifact root from the
`CollaborationAssignment`, the post-Selection preparation factory receives the
live `CollaborationContext`, and a missing root fails closed before runner
creation. The factory is not yet installed by the formal native executable and
does not yet perform canonical fetch/assembly, so F01 remains unresolved and
the overall verdict stays `BLOCK`.

## T037 focused-gate closure — 2026-08-31

The ordinary-V3 repair now includes the required fresh-process negative matrix,
not only in-process assertions. Seven subprocess cases reject V2, hybrid,
tensor degree two, missing roles, duplicate Provider ownership, `TENSOR_RANK`,
and non-zero rank; positive one-, two-, and four-role `PreSplitFirst` cases and
the public request pre-publication gates remain covered. The focused result is
recorded in
[`t037-ordinary-v3-boundary-20260831.md`](t037-ordinary-v3-boundary-20260831.md)
(`17 passed`). F02 and F03 are therefore closed for their repair gates. This
does not change the audit verdict: F01 and F04--F08 still require production
repairs, followed by one fresh convergence audit before formal validation.

## Re-audit checkpoint after the T038 source-boundary repair slice — 2026-08-31

This checkpoint supersedes the earlier F01 wiring description above; that text
is retained as the pre-repair evidence. The production executable now installs
`NativeProviderHandlerConfig::runnerPreparationFactory` in its serving path,
sets `allowPreassembledV3Compatibility = false`, and no longer calls
`materializeManifestSpecs()` while installing formal serving runners. The
factory receives the live authenticated `CollaborationContext`, obtains the
assignment-bound root, fetches the source Data named by the signed root, and
invokes the existing certified Python ONNX assembler. The native assembler also
now requires `canonicalSourceDataName`, `canonicalSourceDigest`, and
`canonicalSourceBytes`, checks the exact fetched size and digest, and signs the
Provider-local cache manifest.

The Python canonical catalog/ensurer now exposes the same source reference in
`CanonicalArtifactBinding`. Strict Spec175 mode fails closed when a legacy root
has no source reference; when the ensurer owns the source payload it publishes
that object before the layer/root publication barrier. Legacy Spec170 callers
remain optional/compatible and continue to use the old layer-only test fixture.

Focused evidence after this repair:

```text
pytest -q tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec170_canonical_layers.py \
  tests/python/test_spec175_v3_boundary.py
38 passed
g++ -std=c++17 -fsyntax-only ... NativeCanonicalOnnxAssembler.cpp
PASS
./waf build --targets=di-native-provider -j1
PASS; build/examples/di-native-provider present
```

This is still not a final convergence `PASS`: no current end-to-end
post-Selection canonical-source fetch/assembly/ORT-load process evidence has
been recorded, and F04--F08 remain controlling. The next audit must verify the
actual authenticated source publication and native Selection-to-runner trace,
then keep formal G0--G3, SIF, and Tiger validation blocked until all findings
are closed.

## Documentation gate correction — 2026-08-31

The pre-test rule is now explicit and consistent across the active feature
documents. `audit.md` distinguishes a structurally/intentionally valid design
from the still-`BLOCK`ed design-to-code convergence verdict; `spec.md` states
that a successful test process is not acceptance evidence until the current
production path is reconciled; `tasks.md` makes a fresh code-aware `PASS` a
hard prerequisite; and `quickstart.md` plus
`.specify/memory/design-code-convergence.md` define the required trace,
repair, focused-regression, and re-audit sequence. Counts were corrected to the
current subject: 82 functional requirements, 21 success criteria, 42 tasks,
26 complete, and 16 open.

The documentation correction does not close an implementation finding. Current
source inspection still records:

* ordinary-V3 placement enforcement and source-bound post-Selection assembly
  as focused repair slices (T037/T038), without a current end-to-end
  Selection-to-assembly-to-ORT process result;
* native coordinator sampling/text output now has a focused T039 repair slice
  (authenticated Greedy/seeded Top-K/Top-P, standalone decoder injection,
  stop matching, and final text). A clean rebuilt native I01 one-Provider
  process and I03 four-Provider process now pass ONNX execution, standalone
  tokenizer loading, ordered token events, and terminal text output. The first
  process attempt exposed and repaired a shared canonical-root binding defect
  and a helper import of the host `_ndnsf.so`; the helper now imports only the
  declared `tokenizers` package and verifies the exact `tokenizer.json` digest.
  Full process coverage is now present for authenticated seeded Top-K/Top-P
  (seed 42, Top-K 3, Top-P 0.95), Unicode-boundary decoding, and split-stop
  matching through the new I16 four-Provider case. The checked-in Python
  standalone-tokenizer oracle and digest-only/empty-delta mutation gates still
  need execution in the qualified runtime, so F04/F05 remain open;
* distributed device-state retention, physical conversation tier movement, and
  filterable production logging as unclosed (T040, reopened T031, T041 open).

## Focused process checkpoint — T039 native text path (2026-08-31)

After a clean rebuild of `build/integration-tests`, the registered production
handlers were run directly through the real Request/ACK/Selection path:

```text
Spec175NativeTinyOnnxI01OneProvider: PASS
  8 ordered token events, terminal Response, cached/full-prefix token parity,
  nonempty textDelta and final decoded text
Spec175NativeTinyOnnxI03FourProviderEpochCoordinator: PASS
  4 Provider completions, 0 Provider failures, 8 ordered token events,
  nonempty textDelta and final decoded text
```

The first attempt was deliberately retained as repair evidence, not as a
passing result. It failed because the shared decode-state authority validated
an empty canonical root on the preassembled compatibility path. Once that
binding was repaired, the native tokenizer subprocess still failed by
importing the full Python adapter graph, which loaded an ABI-incompatible host
`_ndnsf.so`. The helper was narrowed to the standalone Rust-backed
`tokenizers` dependency; a direct digest-bound helper check and both process
cases then passed. This is exactly the required inspect -> repair -> focused
regression checkpoint. **Historical status at this checkpoint (before the
2026-09-01 closure updates):** T039's seeded sampler/Unicode/split-stop
process oracle and T040/T031/T041/T042 were still controlling gaps, and no
formal G0--G3 or SIF/Tiger work was authorized.

Verification of this correction:

```text
audit_speckit_structure.py --strict: PASS (82 FR, 21 SC, 42 tasks, 26 complete)
spec175_contract_gate.check_documents: []
git diff --check (active gate documents): PASS
```

Therefore the current status remains `BLOCK`: only focused repair checks and
compile checks may run, while complete suites, MiniNDN qualification, SIF
replay, and TigerCluster jobs remain blocked until a fresh code-aware audit
reports `PASS`.

## T039 focused source-convergence checkpoint — 2026-08-31

The production source now carries sampling parameters from the authenticated
V3 Selection projection into `NativeEpochCoordinator`, executes the sealed
Greedy or deterministic seeded Top-K/Top-P policy, and rejects unsupported
sampler values during plan decoding. The terminal role accepts a digest-bound
standalone `tokenizer.json` decoder; the serving executable wires it from
`NDNSF_DI_TOKENIZER_JSON` (or `--tokenizer-json`) and requires text output for
formal streamed generation. The exact-SIF launcher passes the external,
read-only model-root tokenizer path without adding PyTorch or Transformers to
the runtime.

The current system NDN-SVS ABI does not expose the newer fetch/piggyback stats
or `subscribeToProducerWithCatchUp` symbols. The framework build was repaired
to use the portable rejection counters and `subscribeToProducer(...,
prefetch=true)` surface; this is recorded as an ABI-compatibility repair, not
as evidence that the optional newer counters exist.

Focused evidence:

```text
pytest -q tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_qwen_generation.py \
  tests/python/test_spec175_streamed_generation.py
39 passed
python3 -m py_compile native_token_decode_helper.py adapters/qwen/generation.py
PASS
./waf build --targets=di-native-provider -j1
PASS; build/examples/di-native-provider present
standalone tokenizer helper smoke (temporary WordLevel tokenizer, exact digest): PASS
```

The native focused regression
`NativeEpochCoordinatorProducesTextAndTerminalFeedback` now also passes after
rebuilding `build/unit-tests`. It exercises the production coordinator and
Provider runtime (rather than a direct sampler helper) for `MAX_TOKENS`
terminal feedback, nonempty Unicode `textDelta`, a stop string spanning token
boundaries, and deterministic seeded Top-K/Top-P replay. This closes only the
development regression for those behaviors; the required four-Provider native
process parity and standalone-tokenizer oracle comparison remain open.

The same rebuilt binary also passes the stateful branch of that regression:
the coordinator marks each stateful epoch for streamed state execution and the
Provider worker observes that authenticated mode. The production CUDA adapter
now retains state outputs in its ORT device map, returns only an opaque local
handle for the decode-state transaction, and clears the session map during
terminal cleanup. This is a focused source/propagation check, not evidence of
physical CUDA transfer counters or a multi-Provider CUDA process; T040 remains
open until those are measured.

## T041 focused logging checkpoint — 2026-08-31

The coordinator, Provider worker, and Provider handler no longer write runtime
diagnostics directly to `std::cout`/`std::cerr`. Their bounded records now use
the shared named `ndnsf.di.RuntimeEvidence` ndn-cxx component with explicit
trace/info/warn/error helpers; existing qualification timing records remain at
WARN so the normal `*=WARN` filter retains them. The source gate
`test_spec175_native_timing_records_use_one_ndn_log_sink` passes and checks the
absence of direct standard-stream diagnostics in those production files.
This is a focused implementation checkpoint only. The following
2026-09-01 closure section supersedes the then-open subprocess/filter/privacy
status for T041.

This checkpoint still does not close T039: a native four-Provider process test
must compare token IDs, text deltas, Unicode and split-stop behavior, and finish
reasons against the standalone oracle. I16 now supplies that native process
coverage with the checked-in Unicode tokenizer, but the Python-side oracle
assertion and digest-only/empty-delta mutation cases still need a qualified
runtime execution. **Historical status at this checkpoint:** T040, reopened
T031, T041, and T042 also remained open, so the convergence verdict and
formal-test block were unchanged. The superseding sections below close T039,
T040, and T041 at their focused boundaries; T031 and T042 remain open.

## T039 seeded process checkpoint — 2026-08-31

`Spec175NativeTinyOnnxI16SeededUnicodeAndSplitStop` passes after rebuilding the
integration binary. The four-Provider process carries `SeededTopKTopP` with
seed 42, Top-K 3, Top-P 0.95, and repetition penalty 1.0 through the signed
Selection projection. It uses the digest-bound
`standalone/unicode/tokenizer.json` fixture (SHA-256
`90db6ef1a0f74a22133269b170a5a4c08a5a4c0d5a76a746614f779e5b2f2404`), emits
token IDs 4, 5, 6 as text deltas `你`, `好`, `🙂`, matches the stop string
`好🙂` across token boundaries, and returns `finishReason=stop_sequence`.
All four Provider coordinators complete with zero failures. A direct Python
`StandaloneQwenTokenizer` load of the same fixture and digest also returns the
oracle text `你好🙂!`. The versioned test class still must run inside the
qualified Python 3.10 runtime; the host Python 3.8 environment is not ABI
authority and fails before collection when it loads the stale host `_ndnsf.so`.
T039 therefore remains open pending qualified-runtime test collection and
digest-only/empty-delta mutation-gate execution; no broad suite or promotion
gate is unlocked by I16 alone.

## T041 expanded logging source checkpoint — 2026-09-01

The source audit was extended beyond the coordinator, worker, and handler to
all streamed-invocation producers under `cpp/ndnsf-di`. `DiTimelineTrace`,
`NdnsfCollaborationDependencyIo`, `DependencyWaitScheduler`, and
`NativeFaultInjection` now assemble bounded single-line records and emit them
through the shared `ndnsf.di.RuntimeEvidence` ndn-cxx component. The existing
marker names and environment switches remain unchanged, so timeline and
dependency parsers can continue to find the records in the logger sink. A
source scan finds no direct `std::cout`, `std::cerr`, or `std::clog` in that
production directory.

Focused verification:

```text
pytest -q tests/python/test_spec175_real_minindn_gate.py \
  -k 'native_timing_records_use_one_ndn_log_sink or uses_bounded_ndn_log_timeline'
2 passed, 62 deselected
./waf build --targets=unit-tests -j1
PASS; build finished successfully (103/103)
./waf build --targets=di-native-fault-provider -j1
PASS; build/examples/di-native-fault-provider linked successfully
```

The fault-provider link also exposed and repaired a production source-list
gap: `examples/wscript` now includes
`NativeCanonicalOnnxAssembler.cpp`, which the executable calls after
Selection. This is a build-wiring correction, not a qualification result.
The subprocess-level filter regression now passes: under
`*=ERROR:ndnsf.di.RuntimeEvidence=WARN`, only WARN/ERROR markers are visible;
under `*=WARN:ndnsf.di.RuntimeEvidence=TRACE`, all four bounded severity
markers are visible. The remaining lifecycle/privacy assertions still belong
to the T042 re-audit; the overall verdict remains `BLOCK` and no complete
suite, SIF, or Tiger result is authorized.

The same source review found one additional unfiltered legacy diagnostic in
`NativeProviderHandler.cpp`: the V1 execution-control import marker used
`std::clog`, which was outside the earlier `cout`/`cerr` scan. It now uses
`logRuntimeInfo(...)`, and the source gate explicitly rejects all three direct
standard streams. The first captured pytest attempt returned an empty logger
stream for the TRACE subprocess; an immediate exact-command reproduction and
the rerun both produced the expected four records. The transient capture was
not counted as a pass; the repeat is the recorded focused result.

## T042 production-path mutation checkpoint — 2026-09-01

The source gate now checks the implementation surfaces that previously could
look green in a broad test while violating the Spec175 design: the fail-closed
post-Selection assembly guard, full authenticated sampling parameters, real
terminal text-delta decoding, Provider-local CUDA state retention, physical
conversation host/GPU transfer hooks, and the registered 1/2/4-role and
M01--M14 production entry points. The new mutation test removes or weakens
each surface and requires its specific gate code; a positive current-source
subject and the earlier source-census mutations still pass.

```text
python3 -m pytest -q tests/python/test_spec175_contract_gate.py \
  -k 'production_path_mutations_fail_closed'
1 passed, 14 deselected

python3 -m pytest -q tests/python/test_spec175_contract_gate.py
15 passed
```

This checkpoint only proves that the cheap gate detects intentional source
drift. It does not prove runtime behavior or formal readiness; T031, T038--
T041, and the fresh T042 code-aware audit remain open, so the overall verdict
is still `BLOCK` and no complete suite, SIF, or Tiger run is authorized.

## Operator process update — 2026-09-01

The pre-test rule is now an explicit per-run sign-off requirement, not merely a
review recommendation. Before accepting any test, qualification, benchmark, or
experiment, the evidence record must contain the five-item card in
`docs/NDNSF-DI-runtime-workflow.md`: frozen design/configuration; CodeGraph and
source trace; discrepancy owner/correction/focused regression; fresh audit
`PASS`; and exact executable/dependency/interpreter/artifact/cwd/argument/
environment/topology identity. An unchecked item is `BLOCK`. Focused red/green
tests may run only to close a named finding and remain development evidence.
Any behavior-affecting change invalidates the prior card and audit identity.

This records the operating decision that implementation/design distance must be
checked and repaired before broad testing, rather than discovered after a
convenient green result. It does not change the current `BLOCK` verdict or
authorize a complete suite, new source seal, SIF, or Tiger run.

## Gate hardening checkpoint — 2026-09-01

The convergence gate itself was reviewed as production tooling after the
process rule was added. Its sampling checks now match identifier markers at a
token boundary, so renaming a field (for example,
`samplingTemperature_removed`) cannot evade the drift check. The text-output
check also requires the delta to be derived from the committed text prefix,
not merely to contain a `textDelta` field. The positive fixture was updated to
represent that exact contract, and the complete mutation suite passes:

```text
python3 -m pytest -q tests/python/test_spec175_contract_gate.py
15 passed
```

This is evidence that the pre-test review process detects two classes of
design/code distance; it is not a runtime or qualification result. The
per-run sign-off card and the overall `BLOCK` verdict remain unchanged until
the open production-path tasks and fresh code-aware audit close.

## Native terminal Python-oracle checkpoint — 2026-09-01

The explicit process oracle required by T039 now runs the real integration
binary from Python's standard library boundary, so it does not accidentally
load the stale host `_ndnsf.so`. It executes I01, I03, and I16 and checks token
IDs, every exposed text delta, terminal decoded text, EOS/stop reason, and the
four-Provider completion/failure counts. The focused result is:

```text
python3 -m pytest -q tests/python/test_spec175_native_oracle.py
3 passed
```

The source gate requires the oracle file and its three production case names.
This closes a development-evidence gap only; it does not satisfy the
qualified Python 3.10/SIF requirement, and the convergence verdict remains
`BLOCK`.

## Post-Selection assembly wiring checkpoint — 2026-09-01

The focused production integration case now exercises a complete V3
Selection with assembly identity and disables the rollback-only preassembled
path. The Provider reaches the normal post-Selection worker, validates the
Selection-bound root/role identity, invokes `runnerPreparationFactory` once
per assigned role, and completes the dependency/Response path (2 roles on
Provider 0 and 1 role on Provider 1):

```text
build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersPrepareRolesAfterSelection \
  --log_level=test_suite --report_level=short
1 test case passed; 25 assertions passed
```

This is a real production-seam regression, not a complete canonical assembly
qualification. **Historical status at this checkpoint:** T038 remained open
until the same path fetched an authorized canonical root and source, invoked
the ONNX assembler helper, signed and validated the assembled manifest, and
recorded the content-addressed cache identity. The subsequent T038 closure
record supersedes this checkpoint.

## Superseding re-audit status — 2026-09-01

The completion graph is now **32/42 tasks closed and 10 open**. This update
supersedes the earlier status paragraphs in this historical audit without
rewriting them:

* T035 and T036 are closed at their non-circular implementation boundaries.
  The current candidate-closure, pre-dispatch, route-probe, terminal-oracle,
  child-reaping, and zero-side-effect tests pass; they do not claim a real
  G0--G4 candidate.
* T041 is closed at its focused implementation boundary. All streamed DI
  producers use the named `ndnsf.di.RuntimeEvidence` component, the complete
  current preflight/route/lifecycle/privacy files pass 94 tests, and the
  subprocess filter fixture explicitly flushes the asynchronous ndn-cxx sink.
  The privacy negative rejects prompt/answer/logit/token/key/content/state
  material while retaining permitted metadata.
* T038, T039, and T040 remain closed at their focused implementation boundaries
  as recorded in their dedicated evidence files; their later MiniNDN, SIF, and
  CUDA qualification layers remain downstream tasks.
* T031 remains the sole unfinished production implementation task. The native
  `ConversationStateStore` still stores a serialized `TensorBundle`; its
  `pauseToHost()` and `prefetchToGpu()` transitions update residency/counters
  but do not invoke adapter-owned D2H/H2D allocation, copy, release, and
  zeroization. The required instrumented transfer seam and real ownership
  calls are not present in the current source.
* T042 therefore remains **BLOCK**. Its production-path mutations and source
  gate pass, but it cannot report a fresh `PASS` until T031 is repaired and
  the public-entry-to-owner trace is rerun against that repaired source.

Evidence for the newly closed focused boundaries is recorded in
`evidence/t035-candidate-closure-20260901.md`,
`evidence/t036-route-terminal-20260901.md`, and the closure section of
`evidence/t041-logging-20260901.md`. No G0--G3, SIF, or Tiger result is
promoted by this update; the old manifests remain bound to their historical
source identities.
