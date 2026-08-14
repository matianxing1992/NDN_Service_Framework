# MiniNDN versus TigerCluster deployment boundary (2026-08-14)

## Purpose

MiniNDN and TigerCluster exercise different acceptance layers. A passing
MiniNDN run proves that the source-level protocol and local harness can make
progress under its declared environment. It does not prove that a sealed SIF,
the deployed Python entrypoint, Slurm wrapper, container mounts, or isolated
multi-process runtime can reproduce that progress. A Tiger failure in one of
those layers must not be reported as an NDNSF protocol failure without a
matching source-level reproduction.

## Observed difference

| Layer | MiniNDN/local evidence | TigerCluster evidence | Interpretation |
|---|---|---|---|
| Source contracts | Python/Spec170 tests passed in the local checkout; native/in-process fixtures use the repository sources directly | The image contains an installed wheel and native binaries built from a sealed source revision | Tiger adds packaging and immutable-image closure |
| Filesystem | Local harness sees repository-relative policy and artifact paths | `--containall` requires the plan's `/bundle` and `/artifacts` paths to be mounted explicitly | A path mismatch is deployment failure, not model failure |
| Python imports | Host `PYTHONPATH`/checkout can expose modules | The actual staged entrypoint must import every transitive module from the installed package | Package-root import or `pip check` alone is insufficient |
| Process state | Fixtures can share a controlled face/event loop | Four Providers need independent `HOME`/PIB/NFD state and distinct child-status checks | Shared state can cause locks or false aggregate PASS |
| Authorization timing | Local tests can hide startup ordering because the event loop and controller are colocated | The Python binding must call `ServiceUser::init()` before asynchronous permission fetch | The 85d7 Tiger smoke reached READY but got `allowed=[]`; this was a binding-order bug |
| GPU/runtime | CPU or local/fake diagnostics do not exercise a container CUDA boundary | `apptainer exec --nv` must expose the allocated GPU and ONNX Runtime CUDA provider | GPU visibility is a separate gate from service execution |
| Orchestration | Pytest/native tests do not exercise Slurm wrapper exit propagation | Wrapper/helper staging, `bash -n`, timeout semantics, cleanup, and per-child status are acceptance inputs | A wrapper can fail or print a false PASS independently of NDNSF |

## Retained Tiger failures

The earlier candidate recorded these distinct failures:

- 189317: `/artifacts` mount topology did not match the plan.
- 189318: the wrapper passed a shell function to `timeout` and did not reach a
  valid user stage.
- 189320: all four Providers became ready, but the deployed user exited with
  `ModuleNotFoundError: ndnsf_distributed_inference.retry`.
- 189336: all four Providers loaded and warmed their ONNX artifacts, but the
  Python user reported `allowed=[]` and exited before request publication.

The last failure led to source revision `d8c605d5557f323530529348cf1c9e590491ef2b`,
which initializes the Python `ServiceUser` before fetching permissions and has
a focused source-order regression. The replacement OCI/SIF and network smoke
must still prove `REQUEST -> ACK -> SELECTION -> RESPONSE`; readiness alone is
not enough.

## Acceptance boundary

The current candidate is accepted in stages:

1. local source and MiniNDN gates;
2. immutable OCI and exact SIF identity;
3. static, CUDA, and standalone model probes in that SIF;
4. real four-Provider Tiger network execution with a non-empty allowed-service
   set and a complete request/ACK/selection/response lifecycle.

Only stage 4 can establish TigerCluster service execution. None of the earlier
stages should be pooled into a success-rate or performance claim.
