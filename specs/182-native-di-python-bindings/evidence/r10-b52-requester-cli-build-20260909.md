# R10-B52 Native Requester CLI Build and Fail-Closed Boundary 2026-09-09

## Batch allocation

本批只验证 `DI_NativeRequester` 的当前 Waf target、动态依赖和 CLI 边界。共同入口是
`examples/DI_NativeRequester.cpp::main`；独立出口是 target 可构建、help/usage/schema
错误按契约返回，并且不会在配置错误时启动 Face 或写 output。真实 Core/Provider 请求、
完整 catalog/grant 配置和 T010/T016 资格留给下一批。

## Coverage matrix

| Lane | Covered files/symbols and check | Result |
| --- | --- | --- |
| production entry/callers | Waf `DI_NativeRequester` target → `DI_NativeRequester.cpp::main` → native catalog/grant/runtime composition | covered |
| implementation/wire | `--config/--input/--output` parser, `ndnsf-di-native-requester-v1` schema gate, error return paths | covered |
| test/harness/oracle | Current binary help, bare invocation and wrong-schema configuration probes; output-file absence on rejection | covered |
| build/source closure | system-first Waf target check with existing Spec182 build tree; requester binary SHA and `ldd` closure | covered |
| migration/evidence | Fresh raw run directory and exact exit/output captures; no Python planner or network process invoked | covered |

## Review trace

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）执行只读审查；
基线为 source commit `7b0e70268b77b117d647b45890a5453859a7b6a8`，范围覆盖完整
`DI_NativeRequester.cpp`、Waf target registration、native binding entry and current CLI
raw probes。逐项检查参数顺序、schema fail-closed、相对路径规则、output 写入时机、
target/source closure 和动态依赖；结论为 `No findings`。

## Verification

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  ./waf -o .codex-tmp/spec182-r4-b2/build build \
  --targets=DI_NativeRequester -j2
-> exit 0; elapsed 1.03s (Waf incremental)

DI_NativeRequester --help
-> exit 0; Usage: DI_NativeRequester --config FILE --input FILE --output FILE

DI_NativeRequester
-> exit 2; usage line on stderr

DI_NativeRequester --config invalid.json --input input.bin --output output.bin
-> exit 1; NATIVE_REQUESTER_FAILED: unsupported requester configuration
```

The binary SHA-256 is
`b668d15571310fc57ef9930edc8d5c7eed5e7352006df3d7d2a1fef85b7898a9`; `ldd` reports zero
`not found`. Raw command output and exit records are retained under
`.codex-tmp/spec182-r10-b52-requester-cli-20260909/`; the Waf log is under
`.codex-tmp/spec182-r10-b52-requester-build-20260909/`.

## Closure decision

`CLOSED_FOR_VALIDATION` for the requester target build and fail-closed CLI boundary.
`OPEN_FOR_NEXT_BATCH` for a valid operator-owned catalog/grant configuration, Core/Provider
transport, terminal native Response, maintained caller migration, conversation and T016.
This focused result does not close T010-B or any parent task.
