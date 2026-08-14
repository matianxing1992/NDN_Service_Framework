# Tiger isolated-PIB permission bootstrap diagnosis — 2026-08-14

## Observed failure

Candidate `runtime-d8c605d5557f323530529348cf1c9e590491ef2b` used SIF
`sha256:12692af7017a8dde626c4332f8b88b3ad4ae0d995aabaf60135497a4ddf56fc7`.
Network smoke job `189390` reached `NDNSF_DI_NATIVE_PROVIDER_READY` for all
four Providers, but the Python user reported:

```text
NDNSF_DI_NATIVE_TRACER_USER_ALLOWED []
missing user permission for /Inference/NativeTracer; allowed=[]
```

No request, ACK, Selection, or Response was attempted.

## Root cause

The Tiger job intentionally gave the Controller, User, and each Provider an
isolated HOME/PIB. The staged policy grants `/Inference/NativeTracer` to the
User and all four Providers, but `ServiceController::getTargetIdentityCertificate`
can only resolve a target certificate from its own PIB (or a certificate issued
by its bootstrap table). It cannot use a certificate that exists only in a
different process's isolated PIB. Therefore the Controller could not encrypt a
PermissionResponse for the newly created remote identities and returned no
Data. MiniNDN regressions did not expose this deployment boundary because their
processes commonly share the prepared local identity state.

This is an identity/bootstrap orchestration failure, not an ONNX, CUDA, model,
or Provider-readiness failure.

## Corrective change

The candidate source now supports the existing certificate-bootstrap protocol
end to end for this harness:

- `di-native-provider --bootstrap-token TOKEN` requests a Controller-signed
  Provider certificate before constructing `ServiceProvider`;
- the Python NativeTracer driver accepts and forwards `--bootstrap-token`;
- User and Provider runtimes honor `NDNSF_CONTROLLER_CERT_FILE` for the actual
  Controller certificate, so isolated PIBs do not invent a mismatched local
  Controller certificate;
- the Tiger job stages a token file, exports the Controller certificate from
  the Controller PIB, and passes the correct token to each identity.

The source change requires a new immutable OCI/SIF candidate. The failed
`189390` result remains negative evidence and is not overwritten.

## Verification boundary

Before a new Tiger submission, Python/driver and shell syntax checks pass
(`50` focused tests). The local full Waf build cannot currently complete because
the checkout's system NDN-SVS headers lack the newer `SVSPubSub` statistics
methods used by `ServiceUser.cpp`; this is a local toolchain/library mismatch,
not evidence that the bootstrap patch compiles in the release image. The
immutable release workflow must therefore perform the authoritative full build
and static probe before another network smoke.

The updated iTiger operations skill now has an executable isolated-PIB
preflight at
`itiger-ndnsf-ops/scripts/validate-isolated-pib-network-inputs.py`.
Against the candidate policy, network job, and Python user driver it returned
`ISOLATED_PIB_NETWORK_PREFLIGHT=PASS` with `ISOLATED_PIB_DETECTED=true`.
The skill text was validated with `quick_validate.py`; its checker output must
be retained with each candidate rather than treating the presence of the skill
file as proof that the gate ran.

The next valid positive result must include, in order:

```text
certificate bootstrap installed
PermissionResponse encrypted and installed
USER_ALLOWED includes /Inference/NativeTracer
REQUEST -> ACK -> SELECTION -> RESPONSE
```
