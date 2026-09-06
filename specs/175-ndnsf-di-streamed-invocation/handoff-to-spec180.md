# Spec175 to Spec180 Handoff

**Prepared**: 2026-09-03
**Source feature**: `175-ndnsf-di-streamed-invocation`
**Receiving feature**: `180-ack-driven-cross-model-qualification`
**Revision**: 110 (explicit delay ownership, finite recovery queue, and
native-library closure gate)
**Handoff status**: `FROZEN_LOCAL_FUNCTIONAL_PASS` for the frozen source subject at source revision `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`; Spec175 T020--T023 are closed for that subject and are not a current-tree pass.
Authoritative closure: `evidence/local-closure-current.md`.

The authoritative source seal is
`evidence/t022-source-seal-current-20260902-r1.json` (SHA-256
`sha256:01e197a39cf6e88ec48945c1963b1849dc97f9a66ab7f37b9ad47f16642ec903`).
Spec180 work added after that seal is a new source delta. It may not be
treated as covered by this handoff merely because the Git revision is the
same or a later dirty-tree seal verifies file bytes.

**One-way boundary**: this handoff closes Spec175. A failure discovered while
implementing or qualifying the current YOLO/Qwen/SIF/Tiger candidate is filed
and repaired in Spec180; it does not reopen Spec175 or trigger a replay of the
frozen Spec175 campaign. Only a deliberate change to a frozen interface or
invariant requires a new Spec180 compatibility task that cites this handoff.

**Current-tree warning**: the handoff's `LOCAL_FUNCTIONAL_PASS` is scoped to
the sealed Spec175 source identity above. It is not a pass for the dirty
working tree or for the later Spec180 YOLO runner. The current Spec180 driver
has a real startup/publication path, but it has not yet produced live NDN
evidence because the candidate package and Provider signing/model inputs are
not sealed and validated. Any repair of those inputs or of the live driver
must remain a Spec180 task; it must not be recorded as a new Spec175 task or
evidence. The receiving runner must also invoke the Controller's
candidate-publication mode explicitly; a repository deployment manifest is not
part of the Spec175 baseline and cannot be used as an implicit publication
trigger.

**Revision 108 operational receipt**: Spec175 is not waiting on an additional
local run. Spec180 is waiting at G0 because no owner-supplied candidate yet
combines current `DetectShard0/1` roles, the registered catalogue signature,
the matching Provider offer key, and the canonical Y-A ONNX object in one
immutable manifest. The correct sequence is G0 input closure -> one real Y-A
terminal Response -> Y-B/Y-N -> QWEN-F/T013 -> T014 -> SIF/Tiger. A missing
input is recorded once as `WAITING_EXTERNAL_INPUT`; it is not repaired by
reopening Spec175, changing the trust root, or rebuilding downstream images.

**Revision 109 native-closure handoff**: A developer Y-A attempt reached the
Controller/Repository startup boundary but both native clients terminated with
socket EOF. The extension/NAC-ABE used a different hashed `libndn-cxx` build
from the one used by NFD/current NDN-SVS. Spec180 now rejects this split
closure before MiniNDN startup (exit 78). The receiving feature must rebuild
NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python extension as one ABI-identical
closure and record the paths/digests. This is not a Spec175 regression or a
protocol result.

**Revision 110 ordered-gate handoff**: The receiving feature must treat the
native closure as `G0-NATIVE`, before the existing candidate-input gate
(`G0-CANDIDATE`). The complete order is:

```text
G0-NATIVE -> G0-CANDIDATE -> G1-Y-A -> G2 -> T013 -> T014 -> SIF/Tiger
```

Spec175 contributes only its sealed local baseline and contracts. It does not
own the native rebuild, YOLO signing inputs, live ACK/Selection/Response, or
any downstream qualification. A `WAITING_EXTERNAL_INPUT` result at either G0
gate must be preserved and resumed at that gate; it must not trigger a
Spec175 replay.

The handoff also preserves the local resource convention: native compilation
uses one active Waf tree at `-j2`. A higher-concurrency or overlapping build is
not an equivalent source/runtime subject and cannot produce qualification
evidence.

**Reason no real Spec180 experiment is inherited**: Spec175 closed only the
generic stream/Qwen CPU-MiniNDN baseline. Spec180's candidate-bound YOLO
vertical slice is a separate source and evidence subject; its current package
inputs are either role-incompatible or unsigned, and its Provider offer/model
inputs are not yet bound to the runner. Therefore no Controller/Repository/
Provider/User exchange has a qualified YOLO terminal Response. This is an open
Spec180 integration task, not a negative result for the sealed Spec175
implementation.

## Receiving-feature entry conditions (revision 105)

Spec180 may start its first live case only after all of these inputs are
available and sealed together:

1. a fresh exporter output using the current `DetectShard0`/
   `DetectShard1` role vocabulary;
2. a trusted catalogue signature and the matching registered trust-root
   digest;
3. a valid Spec180 case policy with explicit runtime nodes, identities, and
   route origins (the historical `/Stage/...` policy is not admissible);
4. an absolute Provider-to-offer-signing-key map and the Y-A canonical ONNX
   path; and
5. one candidate/source/configuration manifest whose hashes are rechecked by
   the barriered runner before any NFD/SVS process starts.

Missing items are an external-input block (G0). They must be reported once with
the required owner and not “resolved” by regenerating a temporary package,
editing a manifest in place, rebuilding SIF, or submitting Tiger.

**Revision 104 reaffirmation (historical; superseded by revision 106,
2026-09-03)**: The host-side QWEN-F manifest check added in Spec180 is only a
release-validation seam. It does not satisfy the missing signed YOLO
candidate, the live Y-A terminal Response, or the separate QWEN-F production
manifest/object set. Revision 106 adds the focused-tested QWEN-F entrypoint;
the remaining gates are still Spec180-owned and ordered G0 → G1 → G2 → T013 →
T014. The frozen Spec175 seal is unchanged.

**Revision 105 scope correction (2026-09-03)**: The receiving feature has one
active gate at a time. It must first close G0 and run one atomic Y-A request to
a terminal Response; only then may it extend the same driver to Y-B/Y-N or
enter T013/T014. This handoff supplies a frozen baseline and entry conditions,
not a parallel work queue. A current-tree dirty result remains an anti-mixing
guard and does not reopen Spec175.

The receiving runner now binds its request/attempt identity to the
`LifecycleJournal` and emits the ten ordered milestones from the maintained
coordinator/User path. This closes only the source-wiring seam; a live Y-A
trace and terminal Response are still required before claiming Spec180
execution or qualification evidence.

The receiving execution order is fixed: G0 candidate/input closure, G1 one
atomic Y-A terminal Response, G2 shared Y-B and fixed Y-N controls, then T013
release/QWEN-F implementation and T014 convergence. SIF and Tiger actions are
not valid recovery steps before these gates close.

## Frozen interfaces and invariants

Spec180 consumes these contracts without redesigning them:

1. One generic Request is published before ACK closure; graph inspection,
   candidate enumeration, placement, artifact preparation, sealing, and
   Selection follow that closed ACK snapshot.
2. A streamed invocation uses one Request/ACK/Selection lifecycle, ordered
   intermediate events, and exactly one terminal Response.
3. One Provider owns one complete role; every role is assigned exactly once and
   one Provider is not reused for another role in the same plan.
4. Qwen generation performs one prefill followed by automatic single-token
   decode; each role owns its complete model state and healthy decode sends no
   model-state tensor over NDN.
5. A continuation binds the exact parent checkpoint, tokenizer/chat-template,
   role map, and context epoch; full-context fallback is explicit.
6. Provider role assembly consumes signed, request/plan/provider-bound assembly
   metadata and verifies canonical object names, bytes, and digests before use.
7. Existing NDNSF authorization, confidentiality, tokens, signatures, and
   replay protection remain mandatory.

## Current production owners

| Capability | Current owner |
|---|---|
| Request-first ACK closure and planning | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py` |
| Model-first public request | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py` and `application.py` |
| Adapter ports and current generic adapters | `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/` |
| Canonical artifact publication | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/canonical_artifacts.py` |
| Native canonical role assembly | `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.*` |
| Provider assembly/runtime wiring | `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.*` and `examples/DI_NativeProviderExecutable.cpp` |
| Qwen prefill/decode and state | `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/` and native epoch/runtime owners |

## Work explicitly transferred

- Replace the synthetic object-detection adapter with a real YOLO26 ONNX graph
  adapter and certified safe candidate catalogue.
- Migrate `examples/python/NDNSF-DistributedInference/yolo_2x2/` from the
  preplanned `distributed_inference()` path to the model-first ACK-driven path.
- Prove that different Provider ACK capability sets select the registered
  atomic or shared-backbone YOLO candidate; do not use caller-supplied role maps.
- Prove real YOLO role materialization, numerical equivalence, and cleanup.
- Create one model-neutral SIF and a Spec180-owned immutable candidate/submit
  interface.
- Run finite real Tiger qualification for YOLO and the frozen Qwen reference
  path with CUDA ONNX Runtime and no CPU model-compute fallback.

## Evidence boundary

- Spec175 local evidence may establish protocol and state semantics only.
- Jobs 206901, 206907, and 207666 are prerequisite diagnostics, not Spec180
  acceptance evidence.
- Job 208200 proves a stale cross-plane candidate can reach readiness and still
  fail before ACK/Selection; Spec180 must retain this as a mutation/closure test
  requirement, not retry that candidate.
- Spec180 starts a new candidate identity. No historical SIF, model bundle,
  profile, launcher, or partial trace is promoted into its PASS result.

## Receiving-contract clarifications

The Spec180 receiver applies these clarifications to the transferred boundary:

- A `REPO_REF` input records `INPUT_REFERENCE_PUBLISHED` before `REQUEST_SENT`;
  only the selected ingress role receives assignment-scoped fetch/decryption
  authorization. The object name alone is not access authority.
- If the registered YOLO candidates are both feasible, signed adapter priority
  followed by candidate-digest tie-breaking selects the plan independently of
  catalogue order.
- CPU ONNX Runtime is valid for local reference/MiniNDN cases and must be
  labelled as such. CUDA and no CPU model fallback apply to Tiger functional
  jobs only.
- Complete local suites are run in supervised process-isolated children. An
  aggregate-process crash is diagnostic and cannot be converted into a pass by
  rerunning one test.
- The exact-SIF Qwen packaging smoke is named `Qwen-runtime-smoke`; it is not
  Q-C evidence and cannot replace the frozen Qwen3.6-27B qualification case.

## Entry condition and invalidation boundary

Spec175 T023 records `LOCAL_FUNCTIONAL_PASS` at the named sealed source
baseline above.
Spec180 may prepare independent YOLO implementation work now, but it cannot
begin formal qualification until it records its source delta, completes its own
convergence audit, and passes its current-candidate local gate.

The Spec175 closure is an inherited semantic baseline, not evidence for the
later Spec180 candidate. Spec180 records its source delta from that baseline;
after its final behavior-affecting change it must run its own convergence audit
and complete local unit, integration, YOLO MiniNDN, and Qwen MiniNDN gate. A
Spec180 change therefore does not rewrite Spec175 evidence, and Spec175 PASS
does not exempt changed code from Spec180 revalidation.
