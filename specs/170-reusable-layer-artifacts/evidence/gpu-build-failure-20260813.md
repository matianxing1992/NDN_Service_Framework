# Spec170 GPU image build failure — 2026-08-13

This is a retained negative build record. It is not a GPU image, SIF, or
TigerCluster candidate.

- Workflow run: `31692859784`
- Source revision: `df517857171158f3f68d4915c1a4e4528d38d72f`
- Foundation input: `ghcr.io/matianxing1992/ndnsf-di-spec170-foundation@sha256:94e0caed7c5675469843fc744a71f6dfd484d59594eb32b042b9288a75d7f15d`
- Build stage: `gpu-assembler`, NDNSF native example build
- Failure: `NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp`
  could not find `<ndn-cxx/encoding/buffer.hpp>` while building
  `di-native-plan-onnx-smoke`
- Root cause: that Waf target declared `BOOST ONNXRUNTIME` but omitted the
  `NDN_CXX` and `NDN_SVS` use dependencies; the locally built foundation uses
  `/opt/ndnsf-di/include`, not a system `/usr/local/include` fallback.
- Candidate status: `INVALID_CANDIDATE`

The correction is `examples/wscript` commit `faaed18`:
`BOOST NDN_CXX NDN_SVS ONNXRUNTIME`. A new GPU workflow identity is required;
the failed image was never pushed or signed.

## Follow-up failure

- Workflow run: `31694109243`
- Source revision: `7514fb7245c3b7e9a7148d6055aed3644fca2f39`
- Failure: the separate `di-native-onnxruntime-smoke` target reached
  compilation but could not find `<ndn-cxx/encoding/buffer.hpp>`.
- Root cause: its Waf `use` list still contained only `ONNXRUNTIME`; the
  previous correction covered `di-native-plan-onnx-smoke` but not this target.
- Candidate status: `INVALID_CANDIDATE`

The next correction adds the same `BOOST NDN_CXX NDN_SVS` dependencies to the
`di-native-onnxruntime-smoke` target. No image was pushed or signed by this run.

## Runtime dependency closure failure

- Workflow run: `31695668300`
- Source revision: `620ff11dcb0d4cac3fb4a1f0e8f5a3a6ab69a1ff`
- Build result: all native targets, including `di-native-onnxruntime-smoke`,
  compiled successfully.
- Failure: the final runtime static probe returned
  `No module named 'cryptography'`.
- Root cause: the GPU lock omitted the application security runtime closure
  (`cryptography`, `cffi`, and `pycparser`) while the owner profiles are
  installed with `--no-deps`.
- Candidate status: `INVALID_CANDIDATE`

The next correction adds those three exact versions from the app-runtime lock
to `oci/locks/gpu.lock`. No image was pushed or signed by this run.
