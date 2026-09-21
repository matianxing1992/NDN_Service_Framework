# B189 r149 SmolLM2-135M stable-cache boundary

## Run identity

- Run: `two-provider-global-r149-smollm135m`
- Candidate: installed `/usr/local/bin/di-native-provider` and
  `/usr/local/libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker`
- Stable root: `/var/tmp/ndnsf-di-native-artifacts/`
- Raw run: `.codex-tmp/spec189-smollm135m-20260920/runs/two-provider-global-r149-smollm135m/`
- Launch log: `.codex-tmp/spec189-smollm135m-20260920/two-provider-global-r149-smollm135m-launch.log`

## Verified scope

The affected standalone production target was rebuilt and installed. Build and
installed hashes matched for `di-native-provider`, `DI_NativeOnnxAssemblyWorker`,
`DI_NativeRequester`, `DI_NativeArtifactAuthority`, and
`libndnsf-distributed-inference.so`. The standalone provider now calls the same
stable-root loader after authenticated Selection and before cold assembly.

The launcher passed each Provider a non-run-scoped cache directory under the
stable root. The namespace is derived from the Provider identity SHA-256, so a
Provider index change does not create a second namespace for the same identity.

## MiniNDN boundary

Both installed Providers reached `READY`, signed ACK, and
`NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY`. No
`CACHE_HIT`, `ASSEMBLY_STARTED`, Selection completion, runner, execution,
terminal response, numerical oracle, repeat, or qualification result was
observed. The Requester produced no response log before the run was stopped
after approximately three minutes of no new production event. The stop was
scoped to the r149 process IDs; cleanup left no active experiment processes.

The r148/r149 catalog uses `protection_epoch=epoch-1`. The new loader
intentionally accepts only `plaintext-v1`, so this run correctly did not reuse a
protected grant-bound ciphertext. The stable root contained only one small ORT
profile file and no assembled model. This is a security-bound cache miss and a
stream/admission liveness boundary, not a cache failure or resource-limit
failure.

## Validation limits

The focused C++ stable-root loader test passed separately, including actual
file digest verification and tamper-to-miss behavior. This r149 run does not
prove warm reuse, two-Provider execution, terminal output, numerical parity,
resource peak, or Spec189 qualification.
