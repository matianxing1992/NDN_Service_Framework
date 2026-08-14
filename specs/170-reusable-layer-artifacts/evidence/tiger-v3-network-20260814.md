# Spec170 Tiger V3 network evidence — 2026-08-14

## Candidate

- source/SIF base revision: `2a7b847bbeba1b628a1a9e2a146b5411f1df5556`
- OCI digest: `sha256:920ccd37d4365fcd8f5423e45112753cd8113163ebabe864b51c0695cdd20b7b`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/runtime.sif`
- SIF SHA-256: `c40cfa9abd964e9feec293093753a3de6fb32327b332bd0a873e15b50cfb6c70`
- V3 diagnostic source commits: `10520f2` and `1d5aafa` (scripts are bind-mounted, not baked into
  the base SIF)
- provider script SHA-256:
  `414419bbd584ce588cb91d598e380b46df71653c0d726e902913b2292f503657`
- user script SHA-256:
  `0def5d06701d51d18e6d60c6a94f78eb9067f666b13ca8b755af3c584846a3d0`
- Slurm job wrapper SHA-256:
  `6dcb5071cae33ce6a33763b8c82a99e01acc0871d091c8c1b844d2f1385910f3`

The base SIF passed the static import, CUDA, Qwen, mount, and isolated-PIB/
network preflights. The base SIF is therefore retained unchanged for this
diagnostic; the V3 scripts are separately hashed release inputs.

## Native path diagnosis

Jobs `189427`, `189428`, `189430`, and `189431` used the old NativeTracer
driver. The real NFD/controller and all four native Providers became ready;
the user received 14 ACK candidates and the selector selected all four roles,
but no Provider Selection/Response was observed before the user deadline.

The failure is not classified as a generic container failure:

1. `native-execution-plan.json` declares `DATA_DRIVEN_V2`, while
   `native_di_tracer/user_driver.py` emits `LEGACY_READY_SET_V1` and calls the
   compatibility `request_collaboration()` path.
2. The native `DATA_DRIVEN_V2` handler requires assignment backend/device/
   artifact fields, which the legacy selector does not produce.
3. The current native V2 readiness check requires CUDA evidence, while this
   job intentionally had no GPU allocation or `--nv`; the Spec170 CPU/no-GPU
   contract is not implemented in that native path yet.
4. A V1 diagnostic variant (`189432`) failed early with the explicit error
   `LEGACY_READY_SET_V1 plan requires --require-execution-lease`, confirming
   that this is a protocol/driver contract mismatch rather than an NFD image
   startup error.

Remote native evidence is retained under:

`/project/tma1/ndnsf-di/evidence/spec170/runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/network-bundle/network-smoke-189427/`

and the corresponding `189428`, `189430`, `189431` directories.

## Python V3 CPU control gate

Slurm job `189434` used the same base SIF, real NFD, the same C++ controller,
four isolated Python Provider processes, and one Python User process. It used
the public V3 lifecycle:

```text
begin_collaboration(DEFERRED)
  -> ACK_CLOSED
  -> PlanSealerV3 / ProviderSelectionProjectionV3
  -> commit_plan
  -> selected Provider handlers
  -> final Response
```

Observed markers:

- four `SPEC170_V3_PROVIDER_READY` markers with `device=cpu`;
- four positive `DI_PLACEMENT_V3` ACKs;
- `SPEC170_V3_USER_PERMISSION ... allowed=5`;
- `SPEC170_V3_USER_ACK_CLOSED ... ackCount=4`;
- `SPEC170_V3_USER_SELECTION_COMMITTED`;
- selected callbacks for Backbone, Head/Shard/0, Head/Shard/1, and Merge;
- `SPEC170_V3_PROVIDER_RESPONSE` from Merge;
- `SPEC170_V3_USER_RESPONSE ... payload=V3_CPU_OK`;
- terminal marker `SPEC170_PYTHON_V3_NETWORK_PASS job=189434 user_rc=0`.

Remote evidence is retained under:

`/project/tma1/ndnsf-di/evidence/spec170/runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/network-bundle/python-v3-network-smoke-189434/`

This is a real-NFD V3 control/selection/response gate, not a full Gate D0:
it uses CPU diagnostic handlers and does not yet prove canonical artifact
publication, Provider-side assembly, model execution, or complete multi-token
output. Spec170 T002–T039 therefore remain open.

## CPU ONNX artifact execution gate

Job `189435` reran the same V3 network flow with `SPEC170_V3_ONNX=1`. Each
Provider loaded its mounted artifact with `onnxruntime` and
`CPUExecutionProvider` before advertising readiness, then executed the model
after Selection:

| role | artifact SHA-256 | output shape | output bytes |
|---|---|---:|---:|
| `/Backbone` | `78933d8d10878d0c1590f04e269c733c40c686595ad92fd15ba78104707ff4bc` | `[1,16]` | 64 |
| `/Head/Shard/0` | `3a8bf108bac8fddfc7edf92f5f26680e33f9c6b0e6de254eebfed43e65e6b0ef` | `[1,8]` | 32 |
| `/Head/Shard/1` | `4cd0bed590c455b44fb5903dc7b996d5216929d40c8fd242a430e8a73dc03a28` | `[1,8]` | 32 |
| `/Merge` | `874a664d460b9bba8f631e1dea8d6342d391364c27c74f5deebe65813d32fd78` | `[1,4]` | 16 |

The User again observed permission, four ACKs, V3 Selection commit, four
selected callbacks, and a final `V3_CPU_OK` Response. The job emitted
`SPEC170_PYTHON_V3_NETWORK_PASS job=189435 user_rc=0`.

ONNX Runtime also emitted repeated `pthread_setaffinity_np ... Invalid
argument` warnings under the Slurm/Apptainer CPU allocation. They did not
change the result, but they are retained as environment evidence; a future
performance run should set explicit ONNX Runtime thread counts before making
latency claims. This job proves real CPU ONNX execution of pre-mounted role
artifacts, not canonical publication, cross-Provider activation dataflow, or
multi-token model generation.

## Next gate

The next implementation gate is to make the real native/Python V3 offer and
CPU execution paths conform to one contract, then replace the diagnostic
handler with the smallest real model/artifact workload. Only after that gate
passes should the Tiger matrix expand to GPU, reuse, cross-Provider, and
hybrid-device cases.
