# T025 single Tiger deployment — 2026-09-02

**Verdict: FAIL; Spec175 remains unqualified.**

This is the one and only TigerCluster deployment for the current Spec175
candidate. It was submitted through the checked-in `multi-provider` launcher
with candidate r5 and Job `208200`. The submission and the exact SIF were
hash-bound before dispatch; no alternate seed, timeout, resource, wrapper, or
second deployment was used.

## Observed result

- Slurm terminal: `208200|FAILED|1:0|00:00:16||itiger02`.
- Gate terminal: `status=FAIL`, `completedProcessGroups=0`,
  `expectedProcessGroups=7`; only `multi-provider-group-00` started and it
  exited with code 1.
- Controller, repository, and the three registered ONNX Providers reached
  their readiness markers. Provider logs report `runtime=qwen-onnx` and
  `NDNSF_DI_NATIVE_PROVIDER_READY`.
- The User opened the request gate, then failed before ACK/Selection and model
  execution with:

  ```text
  RuntimeError: automatic planning candidate digest mismatch
  ```

- No streamed prefill/decode, terminal Response, or functional deployment
  verdict was produced. The complete remote output remains at the candidate-
  bound path under the r5 submit repository and is not mixed with local PASS
  evidence.

## Classification and stop rule

The first failing layer is **source/runtime control-plane metadata**, not
CUDA, SIF ABI, Provider readiness, or Tiger scheduling. The exact SIF locally
reproduces the same mismatch: the planning manifest candidate digest is
`sha256:fa58b0b5ec0317465080ed161f52a09d5e3ecceb22ab54520b26d4d86e750ee1`,
while the current runtime recomputes
`sha256:575fdf9e63c7b4147b52bad17825773037b1d3c70ddf17a29a3563911f66a0f9`.
Graph and adapter-composition digests match, so the manifest/runtime
canonicalization contract is stale or inconsistent.

This failure closes the one-deployment attempt as negative evidence. Do not
submit another Tiger job from Spec175. Repair the manifest-generation/runtime
contract at the cheapest local owner, add a focused regression, and create a
new explicitly authorized candidate only if a later Spec decision requests a
new deployment.

