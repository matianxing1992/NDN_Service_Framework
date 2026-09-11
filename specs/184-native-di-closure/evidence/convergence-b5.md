# Spec184 B5 Design-to-Code Convergence Audit

**Date**: 2026-09-11  
**Status**: `PASS_FOR_T007_PRECONDITION` / convergence is current; qualification remains `PARTIAL`  
**Candidate**: `sha256:f7ee8f65a67a375f993e4db3a7558f22e996b8b170415b7d1325be89e3f32441`

本审查确认当前候选的 Spec、plan、tasks、contracts 与产品 C++ 接线一致，并把未完成的
运行时和外部资格边界保留下来。`PASS_FOR_T007_PRECONDITION` 只表示可以开始候选绑定的
qualification；它不是 `QUALIFICATION_PASS`、`PROMOTION_PASS` 或外部实验通过。

## Scope and source boundary

审查范围覆盖 `specs/184-native-di-closure/{spec.md,plan.md,tasks.md,contracts/}`、
`NativeInferenceClient::request`/`dispatchOperation`/`beginCoreRequest`、
`planNativeRequest`、`NativeRequestPreparation`、`NativeProvider::serve`、维护入口、
测试注册、构建闭包、所有权/锁顺序/清理和失败分类。产品源基线是
`Experimental` commit `760724683a1d73ccc4498a25fc314074e230714a`；候选契约明确排除
工作树中预先存在的 `docs/failure-log.md` 与 integration marker 修改。

## Five-lane coverage

| Lane | Reviewed inputs and command | Result | Remaining boundary |
| --- | --- | --- | --- |
| Production callers | `contracts/caller-matrix.md`; `rg` caller inventory over `examples/`, `Experiments/`, `NDNSF-DistributedInference/` | `PASS`：YOLO、Qwen、provider 和 collector 的 native/explicit-compatibility owner、selector、rollback 与 zero-use 规则均有记录 | D2b/D2h runtime response、真实模型和 Python retirement 仍未资格化 |
| Implementation and wiring | CodeGraph query `timeout 30s codegraph explore "NativeInferenceClient NativeInferenceProvider NativeRequestPreparation planNativeRequest"`; source review of `request` → `dispatchOperation` → `beginCoreRequest` → preparation/planning/serve | `PASS`：native request path、preparation/planner、provider host 和 handler 接线与设计契约相符；兼容构造器仍按契约 fail-closed | 默认无完整 runtime 的构造器仍返回 `NATIVE_REQUEST_PIPELINE_NOT_READY`，不能当作生产链已接通 |
| Test, harness and oracle | registered C++ selectors in `tests/wscript`; B5 component selectors; full unit/integration logs; Python harness regression (`71 passed`) | `PASS` for registration and observed component exits; Python 仅编排/观察 | process/no-Python driver 的当前 manifest schema 不同，negative collector rows 和 parser-fuzz 仍未运行 |
| Build and source closure | Waf candidate build `312/312`; `nm -C` exported symbols; `readelf -d` RUNPATH; current executable/shared-library hashes in `contracts/promotion-candidate.md` | `PASS` for the frozen local build closure and native symbols | `_ndnsf.so` is stale and excluded from no-Python qualification; examples and candidate trees remain separate locked builds |
| Migration and evidence | `contracts/transfer-matrix.md`, `contracts/qualification-matrix.md`, promotion candidate, B1–B5 evidence and failure log links | `PASS`：80-row matrix、candidate identity、external-owner boundary 和 evidence links agree | 73 rows `PARTIAL`, 7 rows `OPEN`; SIF/Tiger is `TRANSFERRED`, not locally qualified |

## Static review trace

只读审查使用 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。审查重点是
`serve`、`makeHostSlotResolver`、`makeLeaseRouter`、`ExecutionLeaseService::handle`、
`ServiceProvider::ServiceRegistration::close` 和对应 Provider-host 测试调用点，检查
ownership、锁顺序、延迟 Face cleanup、过期 host、runtime handler 对 host-owned table 的
引用以及 selector/error mapping。

静态 finding 的处置如下：

1. `HostState -> Target -> ExecutionLeaseService -> resolver -> HostState` 的 self-cycle
   已改为 resolver 内锁定的 `weak_ptr`，并保留 handler 对 host 的强引用；最终路径没有
   未处置 finding。
2. 将所有捕获改为 weak 的诊断方案在无抑制 ASan 下触发 handler 对 host table 的 UAF，
   已拒绝且未计入结果。
3. 初始 examples target 名称错误导致 Waf setup boundary，原始日志
   `.codex-tmp/spec184-b5-examples-rebuild-20260911.log` 保留；随后使用实际 target
   `DI_NativeRequester,di-native-provider` 成功重建。

## Dynamic and full-sweep disposition

Provider-host 的无抑制 ASan/UBSan 和独立 TSan 8-case selector 均通过；B1 TSan、B2/B3
ASan/UBSan 继续作为共享状态机的批次证据。当前代码树没有注册给 Spec184 的 C++ parser-fuzz
target 或 corpus，因此该 profile 是 `NOT_RUN`，不能写成 `DYNAMIC_PASS`。完整 unit 在
`.codex-tmp/spec184-b5-full-unit-20260911.log` 中 exit `0`，而完整 integration 在
`.codex-tmp/spec184-b5-full-integration-20260911.log` 中 exit `1`，共 48 个 Boost failures。
首个运行时边界是 legacy D2b/D2h121/D2h212 的零 response/role，以及 Spec175 tiny-ONNX
的 `stream event gap exceeded retry budget`；这些结果没有被重分类为协议 PASS。

因此本审查的结论是：**当前实现与接受的设计和接线收敛，T007 可以开始候选本地资格运行；
完整 integration、process/no-Python、负例、parser-fuzz 和外部 SIF/Tiger 仍保持开放，Spec184
不能在此记录上标记完成。**
