# T021 candidate-SIF runtime probe

**Date**: 2026-08-23
**Status**: `BLOCKED_EXPECTED` for the existing candidate

The strengthened Spec175 preflight was run against the existing local
candidate `<local-scratch-root>/spec175-deployment-inputs/runtime.sif` using a freshly
prepared source seal that contains the checked-in workload:

```text
python3 packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight \
  --source-seal <fresh-source-seal>/source-seal.json \
  --workload packaging/ndnsf-di-container/jobs/spec175/workload.json \
  --sif <local-scratch-root>/spec175-deployment-inputs/runtime.sif \
  --apptainer /usr/local/bin/apptainer
```

The candidate passed the container-native ABI checks:

- Python `3.10.18` and `_ndnsf.cpython-310-x86_64-linux-gnu.so` import;
- extension and `di-native-provider` `ldd` closures have no `not found` entries;
- ONNX Runtime `1.20.0` exposes `CUDAExecutionProvider`.

It was nevertheless rejected because the deployment image exposes
`functorch` under `/opt/venv/lib/python3.10/site-packages`.  The Spec175
deployment contract allows ONNX Runtime and standalone `tokenizers`, but no
PyTorch/Transformers runtime residue.  The candidate therefore remains
ineligible; this is evidence that the new probe catches a stale base/runtime
image rather than a reason to weaken the rule.  A new local SIF must remove the
residue and pass the same probe before G4/G5.

The tracked build-boundary validator now also requires the final definition to
remove `torch*`, `transformers*`, and `functorch*`.  The current temporary
Spec175 definition was updated accordingly and validates as:

```text
status=PASS
definitionSha256=sha256:93ef8497e1e15c16e11a67d188cee573946ca338bda10a52ee7cc4725e58dd67
```

This does not rehabilitate `runtime.sif`: its embedded runtime still contains
`functorch`, so the old SIF remains rejected until a fresh local build and the
post-build in-SIF probe both pass.

## Replacement candidate r27

The first replacement build (r25) was intentionally not promoted after its
container-native link failed: the direct `di-native-provider` source list did
not include `InvocationStream.cpp`.  After adding that file, r26 reached the
Provider link but exposed the same class of source-closure error for
`NativeEpochCoordinator.cpp`.  Both failures were corrected in the tracked
`examples/wscript` source groups before generating the r27 source seal; no
runtime overlay or host-built binary was used.

The r27 candidate was built with local Apptainer 1.3.4 from the sealed source
subject:

```text
sourceSealDigest = sha256:db303600d7523c109b255879b9ed4b8967a642a99d49b50d82d5f9daceca980b
definitionSha256 = sha256:7764596db56e3243c2404d91a4c79cc387475e5da4c5b50363b5164e5740be42
sifSha256 = sha256:b957f7a5fd1ceef135c2fa9ad548f187f74fbac1389e0ab5f6bed9221f43ba60
recordDigest = sha256:f519e8709f7f5f88d39b76749a0c3a3b65a48c6224fdc6674aeb2f624401a88b
```

The independent preflight and immutable-record validator both returned
`PASS`.  The in-SIF runtime probe reported Python 3.10.18, the exact
`_ndnsf.cpython-310-x86_64-linux-gnu.so`, ONNX Runtime 1.20.0 with
`CUDAExecutionProvider`, no `torch`/`transformers`/`functorch`, and complete
`ldd` closure for both the extension and `di-native-provider`.  The candidate
is therefore a valid local SIF for the next gate, but it is not yet a promoted
G3/G4 artifact: G0 is still blocked by the dirty source subject and G2 still
lacks the registered I12 process case.

The SIF preflight regression now also checks that both direct native Provider
targets retain `InvocationStream.cpp` and that the shared native source group
retains `NativeEpochCoordinator.cpp`, preventing these two link-closure
omissions from silently returning in a later build.
