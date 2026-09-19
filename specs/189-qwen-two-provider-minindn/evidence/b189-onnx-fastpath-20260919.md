# Spec189 external initializer range fast path and r44 boundary

## Scope and static gate

The first unmet implementation task remains T003/B189-1c. The frozen production
diff is limited to
`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`.
It adds a shared `validatedExternalByteRange` parser and a pointer/length SHA-256
overload. `inlineValidatedExternals` uses the shared parser. `buildTensorIndex`
now hashes authenticated external numeric ranges directly after exact
`rawByteLength` validation; INT4/UINT4 retain the existing materialize-and-nibble
normalization path. No protocol field, manifest schema, build dependency, or
NAC-ABE build boundary changes.

The official read-only `review-agent` gate returned `STATIC_PASS` for the frozen
snapshot. The review checked the five lanes: call connectivity and identity
contract, compile/link closure, runtime selector coverage, ownership/resource
behavior, and compatibility/security. Review skill SHA-256:
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.

## Local C++ verification

The existing global Waf tree `build-spec189-b189-3-global-r3` was reused with the
system-first toolchain and `-j4`:

```text
unit-tests target: 25.645s, compile/link PASS
Spec182OnnxIdentity: 13/13 PASS
NDNSF DI production/requester/provider/worker/oracle targets: 4m22.913s, compile/link PASS
```

`ExternalInliningRulesAndInlineEquivalence` exercised the changed identity path
with a non-zero offset, omitted length, explicit `length=0`, range rejection and
invalid locations. No Core, Repo or NAC-ABE target was rebuilt by this unit.

## Real Qwen r44

The previous r39 preparation-timeout record was retained. A new candidate run
used the rebuilt native binary identities and a new run root:

`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r44/`

The maintained root MiniNDN runner reached Controller, Authority, both NFDs and
both Providers. Both Providers emitted signed `ACK_DECISION` offers and
`NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY`. The first
production boundary after the fast path is:

```text
NATIVE_REQUEST_STAGE_FAILED code=NATIVE_STREAM_FAILED boundary=stream
NATIVE_STREAM_FAILED domain=provider boundary=stream message=Core stream failed: stream event gap exceeded retry budget
```

The supervisor recorded `cleanup=PASS`, no remaining processes, and return code
`120`. Resource samples recorded a peak aggregate run RSS of `3619823616` bytes,
a minimum available-memory sample of `3538112512` bytes, zero swap used and zero
swap-I/O delta. This is a stream/coordination boundary, not a qualification
result. No terminal response, provider runner completion, output oracle, repeat
request or `QWEN_TWO_PROVIDER_PASS` was observed.

The fast path therefore removes the former preparation-timeout boundary for this
candidate, but T003 remains `PARTIAL`; the next task is to diagnose the post-grant
stream gap while preserving the same candidate and resource gate.
