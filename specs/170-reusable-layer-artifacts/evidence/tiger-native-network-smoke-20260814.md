# Tiger native network smoke — 2026-08-14

## Scope

This is a candidate-bound diagnostic for the Spec170 NativeTracer wiring on
TigerCluster. It is not an end-to-end NDNSF-DI success result: the user driver
did not reach request/ACK/selection/response because the candidate SIF omitted
Python modules imported by the staged driver.

Candidate binding:

- source revision: `a46abe9110ff816145a42b42e9b365d847e41135`
- OCI digest: `sha256:f1c8288e26d3dd7700b9519e51296fc3ed17027c60780fac4b361f470c628a26`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/runtime.sif`
- SIF SHA-256: `5e556c17492957d07cde1debad5f7d93d794d43f87b3186914065d53e812fb4c`
- plan digest: `sha256:a31c443ba04da7a66b4457fe859d5b63d2d773e325b991c0036891c8a812d216`
- manifest digest: `sha256:1a9f689551796d990f4f2648e97d3120d9be4ef1dc576d8a41390831f38edbfe`
- user driver SHA-256: `0132b327a08bf79a7c231d9d003ae891b48d4d17eb708a1d1fa34449706d1bbb`

The job used the real SIF `nfd`, `App_ServiceController`, four
`di-native-provider --serve` processes, and a Python user driver. Provider
artifacts were mounted under the plan-declared `/artifacts` path.

## Chronology

| Slurm job | Result | Evidence |
|---|---|---|
| 189317 | failed setup | Initial bundle mount did not match the plan's `/artifacts` paths. This was a mount-topology error, not a model failure. |
| 189318 | partial diagnostic | Corrected mounts; NFD, controller, and all four Providers started and loaded/warmed their ONNX artifacts. The wrapper still attempted `timeout ... container`, which cannot execute a shell function. |
| 189319 | failed orchestration | Same wrapper mistake surfaced as `timeout: failed to run command 'container': No such file or directory` in `user.log`. |
| 189320 | failed user stage | Explicit shell invocation fixed the wrapper. All four Providers reached readiness, but the user exited before publishing a request with `ModuleNotFoundError: No module named 'ndnsf_distributed_inference.retry'`. |

Remote evidence is retained under:

`/project/tma1/ndnsf-di/evidence/spec170/native-tracer-check-20260814/network-smoke-189320/`

## Positive result boundary

Job 189320 independently recorded, for Backbone, Head/Shard/0, Head/Shard/1,
and Merge:

- `runnerKind="onnxruntime-cpu"`
- `realCompute="true"`
- `loadCompleted="true"`
- `warmupCompleted="true"`
- `cpuFallbackUsed="false"`
- `NDNSF_DI_NATIVE_PROVIDER_READY`

The controller reported `ServiceController started...`, and NFD produced a
status file. These facts prove candidate-bound local runtime and process
wiring only. They do not prove user authorization, ACK matching, Provider
selection, response delivery, latency, or multi-Provider completion.

## Corrective action

The app wheel's `setup.py` now includes the three modules imported directly by
the NativeTracer user driver:

- `ndnsf_distributed_inference.retry`
- `ndnsf_distributed_inference.runtime_v1`
- `ndnsf_distributed_inference.runtime_v1_evidence`

The static probe and both OCI Python checks import the same modules. A wheel
inspection confirmed all three `.py` files are present, and the focused
container suite passes (`31 passed`). This correction requires a new source
revision, OCI digest, SIF, and release manifest; the old `a46abe9` candidate
must not be reused for the end-to-end gate.
