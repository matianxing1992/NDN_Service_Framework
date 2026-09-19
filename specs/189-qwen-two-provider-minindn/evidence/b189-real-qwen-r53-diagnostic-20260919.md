# Spec189 real Qwen r53 diagnostic evidence

**Date**: 2026-09-19  
**Run**: `two-provider-global-r53`  
**Candidate/build**: the existing global-r3 candidate and build tree; no
production source change was made for this run  
**Status**: `UNQUALIFIED`

## Purpose and command boundary

r53 was a diagnostic retry after r52. It kept the same real two-Provider
MiniNDN workload and enabled `NDNSF_DI_RUNTIME_TIMING=1`,
`NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE=1`,
`NDNSF_COLLAB_LARGE_FETCH_TIMING=1`, and
`NDNSF_DI_DEPENDENCY_OBJECT_TRACE=1`. The Provider CLI carried
`NDNSF_DI_DEPENDENCY_FETCH_TIMEOUT_MS=900000` in both processes. This was a
fresh run with request
`/NDNSF/DI/REQUEST/6f0f11f3cb11312aa42af8a976b4fcc7-1`; it did not reuse a
previous run result or change the model/candidate bytes.

The run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r53/`.
The original launch and process logs remain outside Git under that run root.

## Observed lifecycle boundary

Both native Providers reached process readiness, ACK decision and grant
verification before assembly. The logs show the repaired dependency budget:

```text
NDNSF_DI_DEPENDENCY_FETCH_TIMEOUT_MS 900000
NDNSF_DI_NATIVE_PROVIDER_READY provider=.../provider-0
NDNSF_DI_NATIVE_PROVIDER_READY provider=.../provider-1
NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION provider=.../provider-0 ... status=1
NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION provider=.../provider-1 ... status=1
NDNSF_DI_GRANT_VERIFICATION {"status":"VERIFIED","boundary":"BEFORE_ASSEMBLY",...}
```

Provider 0 created an assembly staging root containing `root.json`, proving
that post-Selection assembly was entered. No Provider log contained a
`RUNNER_READY`, output, or terminal marker. The enabled timing/trace variables
did not produce a completed provider stage marker in the captured process
logs; this is missing observability, not evidence that assembly completed or
that the Provider was idle. Provider 1 likewise did not produce a completed
assembly/runner marker.

The requester ended at the same Core stream boundary:

```text
NATIVE_REQUEST_ROUTE=Runtime.open->User.prepare->PreparedModel.request
NATIVE_REQUEST_STAGE_FAILED code=NATIVE_STREAM_FAILED boundary=stream
NATIVE_STREAM_FAILED domain=provider boundary=stream message=Core stream failed: stream event gap exceeded retry budget
```

The supervisor recorded `returncode=1`, `cleanup=PASS`, and no remaining
processes. No stage output, terminal response, drain, MiniNDN qualification,
or Tiger qualification was observed. The failure is therefore still a
post-Selection liveness/observability boundary; it is not classified as an
ORT failure, Repo corruption, or a reason to increase the timeout blindly.

## Five-lane result

| Lane | r53 result |
| --- | --- |
| Static | `NOT_STATIC_PASS` for a production fix; this run only reused the reviewed candidate |
| Compile/link | `UNOBSERVED` in r53; no source change required a rebuild |
| Runtime-test | `FAIL` at requester stream event-gap; cleanup passed |
| Protocol/qualification | `UNQUALIFIED`; no terminal response or output |
| Resource/observability | `PARTIAL`; supervisor cleanup passed, timing variables did not expose completed stage markers |

The next implementation unit must define a bounded, authenticated progress or
admission-state transition for silent post-Selection assembly, add a C++
counterexample/assertion, pass the frozen read-only static review, and then
repeat the affected native build and fresh real run. Changing only the Python
timeout, retry count, run id, or log level does not close this boundary.

## Build-system ownership note

The root NDNSF Waf is responsible only for NDNSF-owned Core, Repo, DI,
examples and tests. NAC-ABE remains an external installed SDK built by the
NAC-ABE project using its own build system; on the current NAC-ABE checkout its
canonical documented path is CMake and its Waf entry is deprecated. NDNSF Waf
does not recursively build NAC-ABE or treat its checkout as a dependency
prefix.
