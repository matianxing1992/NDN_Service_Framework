# B2 Preparation Evidence

**Spec**: 185-prepared-model-runtime

**Batch**: B2 (`T003 → T004`)

**Status**: `CLOSED_FOR_VALIDATION`

**Validation date**: 2026-09-12/13 (America/Chicago)

**Base**: `9bdde3cf0a32ff3cf8ea1fb541dff5d2d6ae90db`

**Final review snapshot**: `.codex-tmp/spec185-t004-review-v25/`

**Snapshot tracked.patch SHA-256**: `2d1f7772efef5a0cdc689e340599a2b752262abb673de698427eda3b52ff7d9a`

本记录是 B2 唯一批次结果，合并 T003 和 T004 的静态门、组合审查、共享构建和 C++ 验收。快照在组合审查期间冻结，包含两个任务新增的未跟踪公共头、实现和测试文件；无关工作树修改未进入快照或本次提交。

## Scope and design binding

- **T003 / C-02**：`CD02`, `F04–F09`, `FN02`, `FLOW02`, `PO02`。`Runtime::User::prepare` 通过 `PreparationSpec` 交给 `ModelPreparationCache`，由 `NativeRequestCatalog` 和 `NativeCanonicalPreparationCatalog` 完成 source、配置、ONNX graph、initializer、adapter 与 immutable `PreparedModelPackage` 验证。`ModelManifest` 保留 canonical graph digest 与 planning graph digest 的区别。
- **T004 / C-02, C-07**：`CD02`, `CD04`, `F06–F10`, `FN02`, `FN04`, `FLOW02`, `PO02`, `PO04`。cache 实现四种 policy、normal/refresh generation、single-flight、独立 waiter deadline/cancel、lease/LRU/byte budget、可靠 completion 和 worker/terminal 生命周期。
- Core 只提供通用 `OperationRuntime` ticket、调度、完成与订阅控制；DI 通过公开 `OperationSubscription::fromControl` 接入，Core 不依赖 DI、ONNX 或 Python。

## Static review trace

审查 agent 使用本机官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，只读读取完整任务快照和周边 caller/test/build 接线。T003 最终快照为 v10（`f0810d21f4ad28aeb71c80b5f791e2a014bf0132b082eae149867c3353427eca`）；T004 经 v13、v15、v17、v19、v20、v21、v22、v23、v25 逐次修复和复审，最终 v25 为 `STATIC_PASS`，无 P0/P1/P2/P3。v18 和 v24 的失败结果保留在 `.codex-tmp/`；v24 的结构化异常复制问题已在 `ModelPreparationCache::waitFor` 修复，v25 复审确认 `DiError`、`OperationError` 和普通异常语义均保留。B2 组合审查使用同一 v25 冻结快照，覆盖以下五 lane：

| Lane | 实际范围与判定 |
| --- | --- |
| production entry/callers | `Runtime::User::prepare/prepareAsync`、`ModelPreparationCache::prepare/prepareAsync/prepareSingle`、`NativeRequestCatalog::load`、`NativeCanonicalPreparationCatalog::State::find`；实际调用者为 `Runtime.cpp` 和 C++ `Spec185Preparation`/`Spec185Runtime` fixture。covered。 |
| implementation/wire | `buildPackage`、`finishJob`、`runJob`、generation CAS、四 policy、lease/LRU/预算、canonical/planning digest、cancel/deadline、Core `fromControl` 和锁/worker/destructor 顺序；无新增 wire 状态。covered；wire N/A 由无 wire diff 证实。 |
| test/harness/oracle | `Spec185Preparation` 14、`Spec185Runtime` 10、`Spec185CoreOperation` 35；T004 的 refresh、lease、async gate、64-slot capacity、dispatch-throw exactly-once 五例；独立 `tests/fixtures/spec182/onnx-planning-graph-oracle.json` 与 native inspector；`tests/wscript` 注册。covered。 |
| build/source closure | `wscript`/`tests/wscript` 的 target 注册，DI/Core definition TU 到 `spec185-preparation-t004`；normal binary SHA-256 `68010e4e6f36aa70888ca611ff962e754ae34c8fa2716301056e29692baf910a`，TSan binary SHA-256 `ad0601d1d577b77f0c9d87e07b859561f8e6101c9007c03a1bbb5edc10add69d`。covered。 |
| migration/evidence | `api-exposure.json`、C-07 public preparation value types、Core→DI dependency direction、tasks 状态与本记录；Spec184 请求/外部资格仍是 scoped dependency，未被 B2 继承。covered。 |

组合审查的 **Batch growth decision**：B2 保持 T003→T004 两个成员。它们共享 Package/cache owner 和同一 C++ preparation selector，但 T004 依赖 T003 的 Package 验证；没有吸收 B3 request 或 Python 工作。组合静态门返回 `B2_COMPOSITION_PASS`，其当时的 **Closure decision** 为 `OPEN_FOR_VALIDATION`；随后完成下述共享构建与动态验证，故本记录最终 **Closure decision** 为 `CLOSED_FOR_VALIDATION`。

## Build and C++ runtime evidence

构建遵循 `C++ production → C++ tests`，使用已核验系统工具链、`-j4`，没有并发竞争构建。普通构建在已验证 `build-spec185-b0c-normal` 中增量完成：`/usr/bin/g++ 9.4.0`、GNU `ld.bfd 2.34`、Boost `/usr/include` + `/usr/lib/x86_64-linux-gnu`；`.codex-tmp/spec185-b2/normal-build-v9.log`，`rc=0`，`27.550s`。TSan 使用独立同 ABI 配置 `build-spec185-b2-tsan`，`/usr/bin/clang++ 10.0.0`、`-fsanitize=thread`；`.codex-tmp/spec185-b2/tsan-build-v2.log`，`rc=0`，`18.868s`（fresh TSan 配置/构建为 5m24.194s，原始日志 `.codex-tmp/spec185-b2/tsan-build.log`）。

普通 C++ selector：

- `Spec185Preparation`: 14/14, `.codex-tmp/spec185-b2/normal-preparation-final.log`, `rc=0`。
- `Spec185Runtime`: 10/10, `.codex-tmp/spec185-b2/normal-runtime-final.log`, `rc=0`。
- `Spec185CoreOperation`: 35/35, `.codex-tmp/spec185-b2/normal-core-operation-final.log`, `rc=0`。

TSan `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`，每个 suite 重复两次；六次均 `rc=0` 且报告 `*** No errors detected`：

- `Spec185Preparation`: 14/14 × 2，`tsan-preparation-final-repeat{1,2}.log`。
- `Spec185Runtime`: 10/10 × 2，`tsan-runtime-final-repeat{1,2}.log`。
- `Spec185CoreOperation`: 35/35 × 2，`tsan-core-operation-final-repeat{1,2}.log`。

验收覆盖了冷/热准备、错误 source/digest/task/initializer、同 key 并发 single-flight、取消隔离、四 policy、refresh 失败旧包可用、活动 lease 阻止驱逐、预算、异步超时、completion 64 槽和 dispatch 异常 exactly-once。准备结果未取得 grant、candidate、Selection 或 Provider runner 副作用。

## Failure boundaries retained and repaired

先前尝试的第一失败边界均保留在 `docs/failure-log.md` 和 `.codex-tmp/spec185-b2/`：Core 编译命名空间/重载与 private `scheduleAt`、fixture 语法、独立 graph identity、Runtime 同锁死锁、unsupported-task fixture 身份，以及 TSan 同步异常对象生命周期竞争。对应修复后分别复跑受影响 selector；其中 Runtime 死锁通过缩短 `acquireCommit` 作用域修复，TSan 竞争通过在 job mutex 内复制结构化异常字段后抛出独立对象修复。一次错误的 Boost selector 参数产生 `rc=200` 无测试运行，未计为产品结果。

## Retrospective and limits

| Gate | Result |
| --- | --- |
| static | `STATIC_PASS`：T003/T004 逐任务门及 B2 组合门；审查期间无控制性缺陷遗留。 |
| compile-link | normal 与独立 TSan target 均 `rc=0`，实际 binary/hash 已记录。 |
| runtime-test | normal 14+10+35 全通过；TSan 三 suite 各重复两次全通过且无诊断。 |
| unobserved | 全树 packaging/install、Python binding、B3–B9 请求/会话/Provider/跨进程资格、SIF/Tiger/MiniNDN 仍未运行；Spec184 外部资格不继承。 |

因此 T003/T004 可在 `tasks.md` 标为 `PASS`，但 Spec185 整体仍为 `PLANNED`，下一个依赖满足批次为 B3/T005。没有启动 SIF、Tiger 或 Python 验收，也没有把静态通过替代为最终资格。
