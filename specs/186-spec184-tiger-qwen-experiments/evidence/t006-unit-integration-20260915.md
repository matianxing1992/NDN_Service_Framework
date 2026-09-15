# Spec186 T006 unit and integration campaign

## Scope

This receipt covers the current-source host Waf test build after the T006
pre-build boundary checks. It is a native test receipt only; it does not
qualify a SIF, MiniNDN, CUDA, Slurm, or TigerCluster run.

| Field | Result |
| --- | --- |
| build tree | `build-spec186-t006` |
| Waf targets | `unit-tests`, `integration-tests`, `di-native-assembly-worker`, and five `spec182-worker-tool-*` fixtures |
| Waf build | `327/327`, exit 0, 19m46.256s after fixture repair |
| compiler/resource boundary | Clang 10, explicit `/usr` toolchain, `-j4` maximum |
| test fixture binding | `NDNSF_SPEC182_BIN_DIR=build-spec186-t006` |
| full unit suite | `1047/1047` cases and `71081/71081` assertions, exit 0 |
| worker protocol suite | `29/29` cases and `393/393` assertions, exit 0 |
| worker activation suite | `9/9` cases and `76/76` assertions, exit 0 |
| integration suite | `171/173` cases, `3693/3695` assertions, exit 201 |

## Integration boundary

The two failing cases are
`Spec170NdnsfDiCoreFlow/Spec182R10B73NativeConfigQwenRealProviderStream` and
`Spec170NdnsfDiCoreFlow/Spec182R10B80NativeConfigQwenRealProviderConversation`.
Both stop at `ndnsf-di-core-flow.t.cpp:6981` because the configured Qwen source
artifact cannot be opened (`sourceFile.good()` is false). The Qwen3-0.6B stage
manifest/model/tokenizer tuple is still absent, and the Qwen2.5 substitute is
explicitly rejected by T008.a. This is an external-input blocker, not a
passing integration result and not a SIF diagnosis.

## Repairs proven by focused reruns

- The five required worker fixtures are now source-controlled and built by
  Waf. A missing fixture raises an immediate `missing required Spec182 worker
  fixture` error instead of silently omitting the target.
- Worker children retain `HOME`, so ndn-cxx resolves the intended PIB rather
  than a checkout-local root-owned `.ndn` directory. The previous child
  `SIGABRT` boundary is gone.
- A non-empty truncated/garbage stdout stream is classified as
  `DI_NATIVE_ONNX_WORKER_PROTOCOL`; an empty clean stream remains
  `DI_NATIVE_ONNX_WORKER_INCOMPLETE`.
- The oversize finalization test uses a 64-byte local cap instead of allocating
  the 8-GiB production fixture ceiling. This removes swap/OOM noise without
  weakening the rejection assertion.

The full T006.a row remains `BLOCKED_AFTER_BOUNDARY` until the two Qwen
integration inputs are supplied and the complete integration suite is rerun.
