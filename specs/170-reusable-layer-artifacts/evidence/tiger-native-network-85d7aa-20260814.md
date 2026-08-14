# Tiger native network smoke for candidate 85d7aa4 — 2026-08-14

## Candidate and bundle

- source revision: `85d7aa475217cabbcfa92ad28612db99cd36e77b`
- OCI: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:7ab382b75a743b6599b4250e5fb3171289e887177304b6de3759d3278fd75d0c`
- SIF SHA-256: `sha256:2318c5c27675fa875d5a437f2942000e4dd8e8f5d8fbb4e889c99aef4c5796b9`
- network job: `189336`, node `itiger01`, exit `12`
- plan digest: `sha256:a31c443ba04da7a66b4457fe859d5b63d2d773e325b991c0036891c8a812d216`
- service manifest digest: `sha256:1a9f689551796d990f4f2648e97d3120d9be4ef1dc576d8a41390831f38edbfe`
- user driver SHA-256: `sha256:0132b327a08bf79a7c231d9d003ae891b48d4d17eb708a1d1fa34449706d1bbb`

The job used a real NFD, `App_ServiceController`, four
`di-native-provider --serve` processes, and the Python NativeTracer user. It
gave every process isolated HOME/PIB state and used the candidate's copied and
hashed policy/manifest/artifact bundle.

## Observed result

The controller started and all four Providers reached
`NDNSF_DI_NATIVE_PROVIDER_READY` with real ONNX Runtime CPU load/warmup and
`cpuFallbackUsed=false`. The user stage then reported:

```text
NDNSF_DI_NATIVE_TRACER_USER_ALLOWED []
missing user permission for /Inference/NativeTracer; allowed=[]
```

No request, ACK, selection, or response was attempted. This is a failed
end-to-end network smoke, not a Provider runtime failure.

## Source diagnosis and corrective action

The SIF's Python binding constructor called
`fetchPermissionsFromController()` before `ServiceUser::init()`. The native
C++ application performs the reverse order (`init()` then permission fetch).
The binding was corrected in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, with a
focused regression test asserting the order. This correction requires a new
source revision, OCI digest, SIF, and network replay; candidate `85d7aa4`
remains static/GPU/Qwen PASS but network-gate FAIL.
