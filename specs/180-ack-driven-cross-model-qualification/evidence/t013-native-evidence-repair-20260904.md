# T007/T013 native execution evidence repair

Date: 2026-09-04. **Overall verdict: T014 BLOCK.** Focused implementation repair,
not qualification. Continues `t013-numerical-repair-20260904.md`; previous
reports and candidate snapshots are preserved. No task checkbox is newly closed.

## Findings and dispositions

| ID | Severity | Current-source finding | Disposition / owner |
|---|---|---|---|
| NE-01 | HIGH | `DI_NativeProviderExecutable.cpp:withExecutionEvidenceContext` accepted GPU UUID from an environment variable; the adapter profile copied metadata instead of querying the selected device. | Repaired: CUDA runtime PCI lookup plus driver UUID lookup; T007/FR-018. |
| NE-02 | HIGH | V3 `runnerPreparationFactory` omitted the profile prefix; the existing adapter finalized any enabled profile during constructor warmup. | Repaired: V3 unique cache-local prefix, capture after bound request run, profile request/attempt identity; T007/FR-018. |
| NE-03 | HIGH | Post-run evidence had no process/request/cache binding, and aggregate readiness could stand in for a role observation. | Repaired: Worker binds a per-result copy, executable emits the original observation; multi-role aggregate completion is cleared; T013/FR-019. |
| NE-04 | HIGH | No candidate-bound terminal collector consumes actual numerical/native/profile/supervision evidence. | Still open under T013; missing terminal result must fail closed. |
| NE-05 | HIGH | Full transient-helper/secret cleanup, real critical Y-N and all candidate planes remain incomplete. | Still open under T011/T013/T014; no formal validation. |

## Production path and ownership

`ExecutionEvidence` remains the shared evidence type. New additive v1 fields
include actual PID/visibility, GPU identity source, request/attempt, fresh
execution/cache disposition and profile request/attempt. Old v1 records remain
readable with empty/zero/false defaults and cannot claim new execution proof.
The factory no longer imports GPU UUID from arbitrary metadata.

For a CUDA runner, `queryCudaDeviceUuid()` resolves the configured runtime
ordinal to PCI bus address, then to a driver device and physical UUID. It
dynamically loads the existing runtime/driver libraries, rejects unavailable
symbols/failed operations/all-zero UUIDs and does not use shell commands.
CPU and native Merge never invoke it. No host CUDA toolkit install or new hard
link dependency was introduced. GPU visibility is reported as configuration,
not used as proof that kernels executed.

V3 assembly now supplies a PID/sequence-unique ORT profile prefix in the owned
artifact cache. `profileAfterRequest` keeps warmup from finalizing the profile;
the first bound request finalizes it after ORT Run. The profile includes warmup
and that request, and records the request/attempt. CUDA-required session options
disable CPU EP fallback. Profile parsing rejects non-CUDA assigned nodes and
kernel events with no provider; provider-less fence events are ignored.

`ProviderRoleWorker` takes the post-run snapshot and binds actual request,
attempt, PID and cache hit on the result copy. Cache hits and unbound legacy
calls cannot claim fresh execution. The existing observer emits
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` with this original record, alongside
the existing aggregate readiness UPDATE. Multi-role aggregation clears
request-completion/profile binding rather than merging several observations
into an apparent single execution. Merge's unchanged native runner receives
the same post-run binding without claiming CUDA or model-runner compute.

The exact contract, compatibility constraints and official API references are
in `../contracts/native-execution-evidence-v1.md`. The Python readiness
dataclass discards these additive fields and is not a qualification consumer.
The collector must parse Boost property-tree string scalars strictly; in
particular, `bool("false")` is not a valid evidence parser.

## Focused tests and compilation

Initial red run: 13 setup errors and one wiring failure because the identity
helper/fields did not yet exist and V3 assembly had no profile configuration.
After implementation, the final command was:

```bash
python3 -m pytest -q --tb=short \
  tests/python/test_spec180_native_evidence.py \
  tests/python/test_spec180_yolo_numerical.py \
  tests/python/test_spec180_tiger_supervision.py \
  tests/python/test_spec180_tiger_contract.py \
  tests/python/test_prepare_local_sif_source.py \
  tests/python/test_ndnsf_di_runtime_v1.py
```

**99 passed in 8.53s.** The native-evidence tests compile the actual
`ExecutionEvidence.cpp` and `CudaDeviceIdentity.hpp` into a tiny probe and use
explicit synthetic CUDA shared libraries. The runtime ordinal 0 maps to driver
device 42 in the test, proving the code uses PCI mapping instead of assuming
identical ordinals. Tests cover environment UUID spoof rejection, each failed
API operation, invalid ordinals, zero UUID, record roundtrip, cache disposition,
non-CUDA/missing kernel-provider rejection and allowed fence events. Wiring
checks verify the actual assembly/Worker/observer source. Source-archive checks
include the production header and exclude both fake CUDA/probe source files.

Host **syntax checks only**, all exit 0:

```bash
# ORT-enabled adapter
g++ -std=c++17 -fsyntax-only -I. -I/usr/local/include \
  -I/opt/onnxruntime/include -DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP \
  NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp
# ORT-disabled adapter
g++ -std=c++17 -fsyntax-only -I. -I/usr/local/include \
  NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp
# Worker
g++ -std=c++17 -fsyntax-only -I. -I/usr/local/include \
  NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.cpp
# Provider executable, using existing build/header configuration
g++ -std=c++17 -fsyntax-only -DNAC_ABE_CMAKE_BUILD \
  -DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP -I. -Indn-service-framework \
  -INDNSF-DistributedRepo/include -Ibuild -I/usr/local/include \
  -I/usr/local/include/nac-abe -I/opt/onnxruntime/include \
  -I/home/tianxing/NDN/ndn-svs -I/home/tianxing/NDN/ndn-svs/build \
  examples/DI_NativeProviderExecutable.cpp
```

Initial standalone Provider probes lacked NAC-ABE's CMake macro/include paths
and the framework include path; after consulting pkg-config and current build
settings the command above passed. No dependency files were changed. An initial
RAII function-pointer attribute warning was removed; final adapter checks were
clean. No rebuilt Provider/library/extension was promoted into a SIF.

Relevant `git diff --check` and strict Spec Kit structural scan pass (25 FRs,
9 success criteria, 20 tasks). These checks are not a full semantic PASS.

## Four evidence layers and scoped readiness

| Layer | What is established |
|---|---|
| Documents | FR-018/019 remain controlling; native observations have an explicit additive contract. |
| Implementation/wiring | Current canonical adapter, assembly, Worker and observer use the new fields/query/profile path. |
| Execution | Real C++ helpers ran against synthetic CUDA APIs; Python focused checks and host syntax checks passed. |
| Measurement | No real CUDA, live Provider/NFD or YOLO model run; no measured hardware/runtime qualification. |

Intent/ownership/task cohesion remain within T007/T013; no extra task chain or
new inference architecture was added. Security is fail-closed at missing device
identity/profile evidence. Migration retains old v1 parsing but does not upgrade
old records into proof. Full runtime validation and candidate closure are not
ready. Later changes invalidate this source checkpoint and require re-audit.

## Next verified continuation

1. Implement T013 terminal collection from original per-role observations,
   profile files, lifecycle, numerical component and observed supervision.
   Require matching role/Provider/boot/PID/request/attempt/plan/artifact/candidate
   identities, fresh non-cache completion and strict string-scalar parsing.
   Three model roles must prove CUDA on the same physical GPU; Merge must not
   claim CUDA. Profile binding must match the actual cold request, not warmup
   or a previous request. Never invent a record to satisfy the terminal checker.
2. Complete transient bootstrap/nfdc descendant/status handling, secret/scratch
   disposal and redaction. These are still separate controlling T013 gaps.
3. Implement real T011 critical Y-N negatives, bind all candidate planes and
   obtain fresh full T014 PASS before T015 or any SIF/Tiger execution.

No model inference, live NFD, container build/replay, remote upload, SSH, GPU
allocation or scheduler job occurred. Existing dirty source and frozen evidence
were preserved; selected source fingerprints below are not a candidate seal.

## Workflow/tool state

Context Mode project and active health passed at entry. Updating plan.md made
its indexed hash stale; the guard rejected a search, so repository authority
was used until the documentation indexes were refreshed. No stale retrieval
was accepted as the checkpoint. CodeGraph was used before source exploration;
its generic name matching includes archived trees, so only canonical current
paths and exact snippets supported claims. Spec Kit implementation/audit and
GSD progress/health were used. GSD's healthy store reports phase36 in STATE but
an older phase05 verification route in its progress projection; neither changes
the explicitly active Spec180 pointer. This report/tasks.md hold the continuation.
ARS and DeepSeek were not used for this implementation-only repair.

## Selected source SHA-256 (not complete source/runtime binding)

| File | SHA-256 |
|---|---|
| `ExecutionEvidence.hpp` | `4d9c4c73ae573fa39d800a7e5c15b51f92dcc89b468e3287f07c9bb09aa137a0` |
| `ExecutionEvidence.cpp` | `3694d6a295cc8513ad537da1d005c2ed0ff723f8d265224cf4a8775eeb98e326` |
| `ProviderRoleWorker.cpp` | `7cd7bfd3a8cd755ed3ce64e463fae5c15a540bdbe885e1e39cd6d227942e8afa` |
| `adapters/onnx/OnnxRuntimeModelRunner.cpp` | `2198981fccae1537ef333ce7a5dff6da671d2c4cce9a860aca8f72bb364b7ffe` |
| `adapters/onnx/CudaDeviceIdentity.hpp` | `0c5782a292435655d760ea178110db57e68bb575d500d7ff7dd45eeb46f4f53e` |
| `examples/DI_NativeProviderExecutable.cpp` | `e686a9c355d4b371687e24b0e5e10e571db6269456bc039fc74ffe34e894df64` |
| `tests/python/test_spec180_native_evidence.py` | `3d00b9ca46d3ea56f7728bceec0cc646225a43a88d448a68cc9b7a05a521f68c` |

The first three basenames are under `NDNSF-DistributedInference/cpp/ndnsf-di/`;
the adapter paths are under `NDNSF-DistributedInference/cpp/`.
