# R10-B46 Real Provider Native Suite 2026-09-09

## Scope and stable exit

本批只复核已经实现的 native requester 到真实 `ServiceProvider` fixture 的本地出口，目标是把
普通 inline、普通 `REPO_REF`、stream、conversation、replacement 和 alternate replacement
六类 selector 的当前结果写入持久证据。它不改变请求 ID、Provider worker 或跨进程契约，也不把
fixture 结果提升为 T016 qualification。

稳定出口是同一已配置的 `integration-tests` binary 能按具名 Spec182 selector 完成结果断言，
并保留单 Provider replacement 的预期 `DI_NATIVE_NO_ADMITTED_PROVIDER` 失败边界。

## Five-lane coverage matrix

| Lane | Evidence |
| --- | --- |
| production entry/callers | `NativeInferenceClient::request` → `ServiceUser` Core collaboration → `ServiceProvider` callback in `tests/integration-tests/ndnsf-di-core-flow.t.cpp`; selectors `Spec182R4B6*`, `Spec182R10B31*`, `Spec182R10B33*`, `Spec182R10B37*` |
| implementation and wire | Existing R4-B6 preparation/grant/admission/Core commit and Provider `publishFinalResponse`; stream branch validates `GenerationTokenEventV1` and `NDNSF-DI-FINAL-V1`; no source changed in this batch |
| test/harness/oracle | `Spec170NdnsfDiCoreFlow/Spec182*` exact suite filter; seven registered cases; `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` emitted only after native result assertions; replacement case checks the expected ACK-closed failure |
| build/source closure | Existing `.codex-tmp/spec182-r4-b2/build/integration-tests`; `./waf build --targets=integration-tests -j4` with system-first `PATH`; no translation unit was invalidated, so the build was an incremental target check |
| migration/evidence | Current `tasks.md` and plan remain aligned; cross-process requester/Provider transport, maintained caller/no-Python and T016 remain open; no Python runtime was used for the native selectors |

## Review trace

只读审查按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）检查 selector 注册、
fixture 分支、结果 oracle、预期负例和 build/source 边界。基线为 `332510b8`；本批没有源代码
diff，因此没有 P1/P2/P3 finding。已有 fixture 的 in-process 限制按下文保留。

## Commands and results

1. `env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH ./waf build --targets=integration-tests -j4`
   在 `.codex-tmp/spec182-r4-b2/build` 完成，exit `0`，Waf elapsed `0.851s`；这是未发生
   源失效的增量确认，不是 fresh-build speed claim。
2. `./.codex-tmp/spec182-r4-b2/build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182*' --log_level=message`
   运行 `7` 个已注册 selector，exit `0`，`ELAPSED_SECONDS=37.97`，输出五个成功业务 marker
   加一个 replacement 预期失败诊断，最终 `*** No errors detected`。
3. `vmstat 1 5` 丢弃首行后 `si/so` 为 `0/0`、`0/0`、`4/0`、`0/0`；没有持续换页，后续
   native build 可按主机策略使用 `-j4`，仍需逐次观察资源。

## Result and limits

本批 `STATIC_PASS`、`BUILD_PASS`、`FOCUSED_BEHAVIOR_PASS`、`CLOSED_FOR_VALIDATION` 仅适用于
上述 in-process native selector 回归。它证明了当前普通/REPO_REF/stream/conversation
组合没有回归，并保留预期 replacement failure；它没有证明独立 requester/Provider 进程、
Provider assembly worker、真实 maintained YOLO/Qwen caller、numeric parity、I01–I08 或
完整 T016 qualification。对应父任务继续保持 `PARTIAL`/`UNQUALIFIED`。

## Batch retrospective

- **Static miss:** 本批没有新增静态漏检；review 仍以真实 selector 和 callback 注册为边界。
- **Compile miss:** 没有；因为源码未变，Waf 只做增量 target 确认。
- **Runtime miss:** 没有本地 selector miss；跨进程和外部资格未运行，不能记为 runtime PASS。
- **Unobserved:** Provider worker、独立 transport、maintained caller/no-Python 与 T016 矩阵仍未观察。

## Closure decision

`CLOSED_FOR_VALIDATION` for the seven local Spec182 native selectors; `OPEN_FOR_NEXT_BATCH` for
cross-process transport and qualification. 下一批应围绕独立 requester/Provider 进程的可重复
配置与 trace/business oracle 单独建批，不能把新的进程生命周期职责并入本批。
