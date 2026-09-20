# Spec189 r140 focused build and secure-erase evidence

## Status

`PARTIAL` / focused compile-link and selector validation only. This record does
not claim MiniNDN, Qwen, terminal-output, repeat-request, or resource-envelope
qualification.

## Review gate

The immutable v4 source-staging snapshot was reviewed read-only and returned
`STATIC_PASS` after the v2 protected-source erase repair, the v3 erase/lease
ordering repair, and the range-backed material validation repair. The snapshot
and review output are retained under
`.codex-tmp/spec189-r139-source-staging-review-v4/`.

## Focused build and selectors

Using `build-spec189-oracle` and the system-first toolchain, the affected native
closure built with Waf `-j4`: `556/556` targets passed, exit `0`, in
`.codex-tmp/spec189-r140-targeted-build-secure-erase.log`.

The focused selectors passed:

- `Spec175NativeAssembly/Spec189MaterialConsumer*`: `2/2`;
- `Spec182CanonicalPublisher/*`: `14/14`, after correcting the test staging
  directory to a private `0700` path;
- `Spec182OnnxActivation/*`: `9/9`;
- `NativeProtectedDirectory*`: `1/1`.

Selector logs are retained under `.codex-tmp/` with the `spec189-r140-`
prefix. The initial publisher selector against the default `/tmp` staging path
failed with `Permission denied`; this is recorded as a test-environment
staging boundary and was not counted as a source failure. The corrected
private-staging rerun passed.

## Installed candidate identity

The six affected targets were installed through
`scripts/install-global-target.sh`: `ndnsf-distributed-inference`,
`di-native-provider`, `di-native-assembly-worker`, `DI_NativeRequester`,
`DI_NativeArtifactAuthority`, and `spec189-two-provider-oracle`.

The installed candidate hashes were recorded in
`.codex-tmp/spec189-r140-install-secure-erase.log`; `ldd` checks for the
installed executables and library found no `not found`, checkout build path, or
`.codex-tmp` dependency.

## Changed boundary

`NativeProtectedArtifactStore` now exposes a pinned-file eraser bound to the
protected directory lease. Canonical ONNX source staging uses that eraser for
overwrite/fsync/unlink and reports cleanup failure instead of bypassing the
registered secure-erase path. The directory lease serializes early file erase
and final drain. `NativeOnnxRecipeAssembler` copies range-backed payloads
before protobuf parsing, so validation no longer passes a null contiguous-data
pointer.

## Remaining gates

The installed candidate still requires a fresh real run through prepare,
manifest publication, ACK, Selection, placement-bound fetch/assembly,
execution, terminal response, cleanup, and repeat-request/resource checks.
T003, T005, T006, T007, and T009 remain `PARTIAL`.
