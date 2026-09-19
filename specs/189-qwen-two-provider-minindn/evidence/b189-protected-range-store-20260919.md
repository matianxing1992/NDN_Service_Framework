# B189 protected range-store and production prepare evidence

**Date**: 2026-09-19 04:52 -05:00
**Status**: `FOCUSED_CXX_PASS` / `T003 PARTIAL`
**Scope**: B189-1a protected Core `ServiceUser` publication through the Repo range
store, plus the existing production Runtime prepare selector. This record does not
claim a Qwen, ACK/Selection, Provider, MiniNDN, or Tiger qualification.

## Frozen static gate

The reviewed candidate is the immutable snapshot
`.codex-tmp/spec189-b189-1a-protected-review-r2/`, based on `df49bb34`.
The frozen diff SHA-256 is
`763668ab9d60ed74ee93280499752b846ff516015915f01037e5f06a3f78cb45`.
The official read-only `review-agent` returned `STATIC_PASS` with no findings.
The review covered Repo operation/generation identity, transactional rollback,
old-lease replacement fencing, Core worker cancellation and serving/key leases,
Runtime injection on both transport paths, requester configuration, test/oracle
registration, and source closure. The reviewer did not build or run the code.

## Compile and link

The existing `build-spec189-b189-3-global-r3` tree was reused. The initial affected
target build used the repository Waf only:

```text
/usr/bin/python3 ../waf build \
  --targets=spec189-encrypted-repo,spec189-prepared-request,DI_NativeRequester -j4
```

It completed with exit 0 in 2m51.563s, with peak RSS 1,801,784 kB and zero swaps.
The affected `ServiceUser.cpp` object was then rebuilt in the same tree and the
same target set was rechecked. The durable log is
`.codex-tmp/spec189-b189-1a-protected-build-r1/build-affected-r3.log`; it records
exit 0, 1m25.86s, peak RSS 2,802,796 kB, zero swaps, and only the existing
`-Wmaybe-uninitialized` warning at `ServiceUser.cpp:5883`.

No NAC-ABE source was recursively built by NDNSF Waf. NAC-ABE remains an external
installed SDK owned by its own build system; NDNSF Waf consumed the installed
`/usr/local` closure. `readelf`/`ldd` showed system Boost 1.71, NDN-CXX/NDN-SVS
from `/usr/local`, and ONNX Runtime from `/opt/onnxruntime`, with no missing
library or `.local-boost171` path. The built artifact hashes were:

```text
spec189-encrypted-repo  5cac64cb91fbad8f99df4695f4bfb113a914a987d8bc44c009044125ef3436aa
spec189-prepared-request 0f887bbd500d5ab3ab3ae5c153279640a593e4a5eee16a3b6b71b73924f7ac5a
examples/DI_NativeRequester a1422c608eaabd131c09254e80b22eaf133ae66f32cb4a48fa19c483dca67ce8
```

## C++ runtime selectors

From the repository root, both selectors were run against the rechecked binaries:

* `spec189-encrypted-repo --log_level=test_suite`: 6/6 cases, exit 0. It covers
  worker I/O responsiveness and cancellation drain, TTL/lease release, duplicate
  input, same-name replacement fencing, stale transaction rejection, and scoped
  cancellation rollback. Log:
  `.codex-tmp/spec189-b189-1a-protected-runs-r1/encrypted-repo-r2.log`.
* `spec189-prepared-request --run_test=Spec185PreparedRequest/Spec189RuntimeUsesProtectedEncryptedRepoPublication --log_level=test_suite`:
  1/1 case, exit 0. It uses Runtime's production Core publisher with
  `RepoEncryptedLargeDataStore`, verifies encrypted source/root/material manifests,
  bounded range reads, receipt bindings, source release, and no object growth on a
  second prepare. Log:
  `.codex-tmp/spec189-b189-1a-protected-runs-r1/prepared-protected-r2.log`.
* `examples/DI_NativeRequester --help` returned the native requester usage and
  schema successfully. This is a link/entrypoint check, not a model request.

## Five lanes and closure

* **static**: `STATIC_PASS` for the frozen protected range-store and Runtime wiring.
* **compile-link**: affected NDNSF targets built with repository Waf and the global
  installed dependency closure; artifact identities and loader paths recorded above.
* **runtime-test**: the two named C++ selectors passed (7 selected cases total).
* **unobserved**: real multi-node Repo/NDN serving, NAC-ABE/controller lifecycle,
  real Qwen source/material pressure, ACK/Selection, selected-layer Provider
  assembly/execution, terminal output, cleanup across processes, MiniNDN and Tiger.

**Closure decision**: the protected local publication/lease boundary is closed as a
`FOCUSED_CXX_PASS`. T003 remains `PARTIAL`; B189-1b material handoff and the later
production ingress and two-provider batches remain open. The NDNSF Waf dependency
boundary is now documented separately from NAC-ABE's own Waf build.
