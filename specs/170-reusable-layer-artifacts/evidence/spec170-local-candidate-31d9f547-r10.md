# Spec170 local candidate r10 (2026-08-17)

## Immutable identity

| Item | Value |
|---|---|
| Release | `spec170-runtime-31d9f547-post-selection-v8-20260817` |
| Definition | `spec170-local-candidate-31d9f547-r10.def` |
| SIF | `/tmp/spec170-local-candidate-31d9f547-20260817-r10/runtime.sif` |
| SIF SHA-256 | `bd949732fc89bb10ef48e92b51fb4557fb33819797882765bd629c73da43748c` |
| Apptainer | local `1.3.4`; Tiger `1.3.4-1.el9` |
| Tiger action | verify hash and execute only; no remote build/materialization |

## Local gates

* C++ integration: **24/24 cases, 323/323 assertions**.
* Dedicated native post-Selection suite: **3/3 cases, 35/35 assertions**.
* Python Spec170 suite: **59 passed, 5 skipped, 1 warning**.
* Real MiniNDN NativeTracer CPU gate: **2 passed** (D0 four-provider and D1
  single-provider); this uses the real process topology and ORT CPU runner.
* Exact-SIF CLI gate: **1 passed**; the workload command-line contract includes
  `--bootstrap-token` and `--execution-policy`.
* Exact-SIF Python import: `ndnsf._ndnsf` imports from the Python 3.10 site
  package inside the SIF.
* Exact-SIF ORT: version `1.20.0`; providers include CUDA, TensorRT, and CPU.
* Exact-SIF provider wiring check: `/Inference/NativeTracer`, four roles,
  four artifacts, four registered runners; final `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`.
* Full packaged-library closure: **PASS**; no host-only RPATH, unresolved
  dependency, duplicate lock row, or unlocked packaged library.

## Failed predecessor retained

Candidate r9 built successfully but was rejected by the closure gate because
the provider/framework/extension retained host-only RPATHs into the checkout
(`.codex-tmp`, `.local-boost171`, and `build`). It is retained as failure
evidence and was not promoted. r10 uses copies with the fixed in-SIF RPATH
`/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib` and was rebuilt from that
explicit input.

## Boundary

These are local source/SIF gates. They authorize hash-bound Tiger execution but
do not count as Tiger D0/D1/D2a/D2b/D2h results. Each Slurm case still requires
its own staged-SIF hash, allocation/device evidence, request/ACK/Selection/
assignment/response lifecycle, and negative evidence where specified.
