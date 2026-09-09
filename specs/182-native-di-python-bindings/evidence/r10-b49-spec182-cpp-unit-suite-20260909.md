# R10-B49 Spec182 C++ Unit Suite 2026-09-09

## Scope and stable exit

本批运行当前已构建的 Spec182 C++ unit-test families，验证 native runtime、protocol、state、
crypto、model、stream/conversation 和 binding-independent contracts 的 focused selectors
没有回归。测试二进制为 `.codex-tmp/spec182-r4-b2/build/unit-tests`，其 SHA-256 为
`f10e68b47d9ba48c7ebe155d09c133f959d8d1f952c8645a96d3ac37c567b0fc`；本批没有源码变化，
因此不重建。

## Five-lane coverage matrix

| Lane | Evidence |
| --- | --- |
| production entry/callers | `unit-tests` registered `Spec182*` suites; includes `Spec182NativeInferenceClient`, `Spec182ClientState`, `Spec182Conversation*`, `Spec182NativePlanning`, `Spec182Preparation`, `Spec182PlanSealer`, `Spec182Onnx*`, `Spec182Grant*`, `Spec182ProviderHost` and related native suites |
| implementation and wire | C++ fixtures directly exercise production targets and canonical wire/state validators; no Python planner, binding facade or offline oracle is used by this selector |
| test/harness/oracle | Boost.Test exact wildcard `--run_test='Spec182*'`; 249 registered cases; process exits only after all assertions and reports `*** No errors detected` |
| build/source closure | Existing Waf `unit-tests` artifact from the Spec182 build tree; no invalidated source or link task in this batch; source identity at run is commit `6c7ba388f4a87e436b1972f3a21b8dcee5bb7571` |
| migration/evidence | This record preserves test command, binary hash, elapsed time and resource observation; cross-process requester/Provider, maintained callers and T016 qualification remain separate gates |

## Review trace

按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）的只读检查核对测试
注册、Spec182 suite 覆盖、C++ production target ownership 和无 Python 依赖边界。本批没有
source diff，未发现 P1/P2/P3。

## Command and result

```text
/usr/bin/time -f 'ELAPSED_SECONDS=%e' \
  ./.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182*' --log_level=message
```

Exit `0`; `Running 249 test cases...`, `*** No errors detected`, elapsed `27.72s`.
这证明当前 C++ unit contract 集合通过，但不能替代真实 NDN requester/Provider 进程、
maintained caller/no-Python 或 T016 matrix。

## Resource observation

独立的 `vmstat 1 4` 快照在丢弃首行后出现 `si/so=528/0`、`4800/0`、`132/0`。该样本显示
测试期间主机存在持续 swap-in；按 `docs/native-build-parallelism.md`，下一次 native build
必须使用 `-j2`，直到新的连续观察证明可恢复 `-j4`。这是资源策略信号，不改变本批测试
结果，也不能把旧的 `-j4`/`-j2` 时间直接比较。

## Batch retrospective

- **Static miss:** none in this no-source-change regression batch.
- **Compile miss:** none; the existing unit binary was reused and hash recorded.
- **Runtime miss:** none among 249 C++ cases; all exited cleanly.
- **Unobserved:** independent requester/Provider transport, maintained caller/no-Python and full T016.

## Closure decision

`CLOSED_FOR_VALIDATION` for the current Spec182 C++ unit suite; `OPEN_FOR_NEXT_BATCH` for native
cross-process integration and qualification. Parent product tasks remain `PARTIAL` until their
runtime and migration exits pass.
