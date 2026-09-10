# R11-B8-G12 Native Provider Identity Binding

**Status**: CLOSED_FOR_VALIDATION（仅限 C++ Provider host identity boundary）
**Date**: 2026-09-10
**Scope**: `NativeInferenceProvider::serve`、`NativeProviderHandlerConfig`、真实 `ServiceProvider` identity 与 shared lease host state

## Finding

静态审查发现 `NativeInferenceProvider::serve` 原先允许空的或与底层
`ServiceProvider` 不同的 `localProviderName`，也允许空的 `providerBootId`。首个
target 会把这些值写入 host-wide lease state，随后用于 offer、execution evidence、
cross-Provider data name 和 lease binding。小型同进程 fixture 可能因此继续运行，
多机部署则会发布与实际 Provider 证书不一致的 capability，或在 Selection、数据获取、
lease/revalidation 边界才失败。

## Change

- `NativeInferenceProvider::serve` 在创建 host state、固定 lease 或 target registration
  前解析 `localProviderName`，拒绝 malformed/empty NDN name，并要求它与
  `ServiceProvider::getName()` 完全相等。
- 拒绝 empty `providerBootId`；后续 target 继续受同一 host boot epoch fence 约束。
- host state 与每个 effective handler config 保存底层 Provider identity 的 canonical
  URI，避免等价 URI spelling 造成 lease/evidence/data-prefix 分裂。
- 新增 C++ negative/positive selector，验证 missing identity、foreign identity、missing
  boot ID 均在 host 创建前 fail closed，合法配置仍可注册；既有 rollback、duplicate、
  re-serve、stop 和 real lease dispatch selectors 保持覆盖。

## Static review boundary

审查覆盖 `NativeInferenceProvider.cpp/.hpp`、`NativeProviderHandler.hpp/.cpp`、
`ServiceProvider::getName()`/`getProviderBootEpoch()` API、native executable config
接线及 host lifecycle tests。未发现 native path 中固定 `127.0.0.1`、localhost 或固定
TCP port；跨 Provider data fetch 已按 endpoint/provider prefix 走 signed exact-data
路径。该批修复的是确认到的 identity binding 缺口，不把静态 absence 当作网络资格证明。

## Verification

All commands ran from repository root with the system-first toolchain and `-j2` after
recorded swap pressure.

| Gate | Command / result |
| --- | --- |
| Fresh C++ build | `./waf -o .codex-tmp/spec182-r11-b8-g12-provider-identity-20260910/build build --targets=unit-tests -j2`; final `190/190`, exit `0`, elapsed `32.22s`, max RSS `1,135,096 KB` |
| Host selector | `unit-tests --run_test='Spec182ProviderHost/*' --log_level=test_suite --report_level=short`; `8/8` cases, `67/67` assertions, exit `0` |
| Full Spec182 C++ regression | `unit-tests --run_test='Spec182*' --log_level=error --report_level=short`; `259/259` cases, `7104/7104` assertions, exit `0` |
| Durable checks | `git diff --check`, `validate_design.py --json`, `verify-spec-kit-sync.py --require-entrypoints --require-personal` recorded at batch close |

The first build attempt failed only in the test alias declaration (`constexpr char[] =
HOST_PROVIDER_NAME`); the compiler diagnostic is retained in
`.codex-tmp/spec182-r11-b8-g12-provider-identity-20260910/build.log` and indexed in
`docs/failure-log.md`. The alias was changed to `const char*` before the checked build;
the failed attempt is not counted as a test result.

## Limits and next step

This evidence closes only the Provider host identity/configuration boundary. It does not
claim cross-machine transport, MiniNDN, full model qualification, maintained caller
migration, no-Python dependency closure, T015, T016, or T017. The next production-chain
batch remains the open maintained callers and independent deployment gates.
