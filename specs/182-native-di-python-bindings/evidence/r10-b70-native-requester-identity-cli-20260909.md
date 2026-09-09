# R10-B70 Native Requester Identity Configuration

## Scope

本批次复核独立 requester/provider 可执行目标的 source/link closure，并修复 R10-B67
身份映射在 CLI 配置入口的缺口。Python facade 已能传递 caller correlation，但
`DI_NativeRequester` 原先没有从 `requester.json` 读取 `application_request_id`，使维护
调用方无法通过独立 native requester 观察 caller wire ID 与 native owner ID 的映射。

## Implementation and static review

- `examples/DI_NativeRequester.cpp` now accepts optional `request.application_request_id`
  and copies it into `NativeRequestOptions.applicationRequestId`; the native client remains
  the sole authority for Core `requestId` allocation and validation.
- `native-requester-configuration.md` documents the optional field as a bounded caller
  correlation, distinct from the authoritative Core request identity.
- The binding source test asserts the CLI mapping is present. Static review covered JSON type
  failure, empty/NUL/length validation at the native client boundary, source registration,
  loader identity and no planner fallback. No new control defect was found.

## Validation

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o .codex-tmp/spec182-r4-b2/build build --targets=DI_NativeRequester -j2
  PASS; requester rebuilt and linked in 13.915 s

env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o .codex-tmp/spec182-r4-b2/build build --targets=di-native-provider -j2
  PASS; provider executable rebuilt and linked in 34.309 s

env LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:\
/home/tianxing/NDN/nac-abe-integration-182/install/lib:\
/home/tianxing/NDN/ndn-svs/build \
  .codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester --help
  PASS; documented usage returned

env LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:\
/home/tianxing/NDN/nac-abe-integration-182/install/lib:\
/home/tianxing/NDN/ndn-svs/build \
  PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_spec182_native_bindings.py
  PASS; 16/16

ldd .../examples/DI_NativeRequester
  PASS; candidate ndnsf-distributed-inference and ndn-service-framework, explicit
  NAC-ABE integration prefix, and ndn-svs build paths resolved; no `not found`

git diff --check
  PASS
```

The first target builds and the Python/source checks ran after the repair. Raw build and
loader outputs are retained under `.codex-tmp/spec182-r10-b69-requester-build-20260909/`,
`.codex-tmp/spec182-r10-b69-provider-build-20260909/`, and
`.codex-tmp/spec182-r10-b70-requester-correlation-20260909/`. The post-build `vmstat 1 2`
showed swap-in activity in the second sample; subsequent native builds remain at `-j2` until
a fresh low-pressure measurement justifies otherwise.

## Boundary and closure

This closes the independent CLI identity-mapping composition boundary. It does not claim a
real native requester-to-independent-Provider process run, two-turn conversation, maintained
caller migration, Python exclusion, or T010--T017 qualification. The next exit remains a
fresh cross-process request with both IDs observed and bound to the same source/dependency
identity.
