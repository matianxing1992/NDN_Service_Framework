# Spec184 B5 Design-to-Code Convergence Audit

**Date**: 2026-09-11  
**Status**: `PASS_FOR_T007_PRECONDITION` / convergence is current; qualification remains `PARTIAL`  
**Candidate**: `sha256:bf301beb053db1a9823b23f767afe051ac95808d5eace8df4c417a263115d931` (fresh local record)

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
ASan/UBSan 继续作为共享状态机的批次证据。fresh candidate 已注册并运行
`Spec182ObservedOffer/Spec184NativeParserFuzz` 的有界 C++ parser sample，normal 与无抑制
ASan/UBSan 均 exit `0`。旧 candidate 的完整 integration 48 failures 和 process/schema
preflight 仍只作为历史边界；fresh candidate 的 unit/integration 已重新运行并均 exit `0`。
I02 单例 ASan 仍以 fixture LeakSanitizer 诊断 exit `134`，不能写成 `DYNAMIC_PASS`。广泛
Waf build 的首个边界仍是未纳入 Spec184 closure 的 legacy `spec181-assembly-parity` 链接目标，
因此只把显式 candidate target closure 作为当前 build lane 结果。

因此本审查的结论是：**fresh candidate 的设计、接线、C++ unit/integration 和 observed-offer
parser sample 已达到 T007 本地资格的前置条件，但仍不能标记 Spec184 完成。**当前进程/no-Python
owner 的非 root 运行保留 `MININDN_REQUIRES_ROOT` 边界；授权 root owner 已关闭 bounded
`PO-001-stream`，但 I02 sanitizer leak、I02–I08 其余行、继承负例/collector、真实模型、
Python retirement 及外部 SIF/Tiger 仍保持开放。

## Fresh candidate refresh

2026-09-11 的刷新绑定当前源码三处生产/测试差异：
`NativeProviderHandler.cpp` 在认证 source alias 与 `APPLICATION_INPUT@request-input`
重名时先移除 alias 再安装 canonical scope；`NativeEpochCoordinator.cpp` 在无
`prepareRunner` 的 preassembled compatibility path 使用已加载 runner 的普通 runtime 入口；
`di-native-observed-offer.t.cpp` 增加固定 seed 的 512-case parser-fuzz selector。官方
`review-agent` 对完整差异及注册调用点给出 `No findings`。

candidate target closure 使用 `.lock-spec184-b5`、系统 `/usr/bin/g++ -B/usr/bin`、`-j4`：
主 targets `502/502`，worker/helper closure `15/15`。candidate-first library path 下，
fresh `unit-tests` exit `0`（日志 SHA-256
`127d751be7522d0f7b3c8b3968bae393ae7d08082debf0b6d6bb65dfe6fcbb4d`），fresh
`integration-tests` exit `0`（日志 SHA-256
`f0fccf442f6de69ab6a5e585e1eb84eb470aa1fad49e2316d6ca0210c2962027`）。运行时显式设置
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`，所以 worker/helper 查找边界已记录。
完整 Waf build 没有纳入历史 `spec181-assembly-parity` 辅助目标；该失败保留在
`docs/failure-log.md`，不作为当前 Spec184 target 的失败。

fresh owner manifest `sha256:4212ca6f81913f30c10c23b1a5d3fcfa6d6a172de295b6e5a67fd3b2c23a7826`
的 `PO-001` 运行首先保留了当前 uid 1000 的 `MININDN_REQUIRES_ROOT` 边界；随后在
`PATH` 补回 `/usr/local/bin` 的授权 root owner 条件下，canonical two-node case exit `0`，
完整 trace/namespace/process/endpoint/cleanup 和 business marker 已记录在
`.codex-tmp/spec184-b5-owner-probe-20260911-r7/`。旧 root owner PASS 没有被重绑定；r7 是
当前 candidate 的新 owner evidence。该刷新为 T007 提供了可复核的本地 C++ 与 bounded
`PO-001` owner 前置条件，但不是 I02–I08、真实模型或 external-owner qualification。
