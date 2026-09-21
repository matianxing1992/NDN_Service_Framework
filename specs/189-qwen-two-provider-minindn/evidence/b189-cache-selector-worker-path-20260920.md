# Spec189 cache selector worker-path boundary — 2026-09-20

The affected `spec185-provider-assembly` binary was rebuilt after adding the
stable-root cache loader and the digest-verification regression. The combined
selector ran the new `ProductionAssemblerCacheScansStableRootAndVerifiesFileDigest`
case successfully. The adjacent pre-existing
`ProductionAssemblerCacheColdHitUsesExactArtifact` case stopped during its
fixture setup with:

```text
DI_NativeOnnxAssemblyWorker binary not found
```

The worker is present at `build-spec189-oracle/DI_NativeOnnxAssemblyWorker`,
but this selector invocation omitted the required
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle` environment. This is a test
invocation/preflight boundary, not a cache or ONNX contract result. The raw
terminal output was produced by the focused selector; the next invocation must
set the explicit worker directory and rerun the old production assembler case.
