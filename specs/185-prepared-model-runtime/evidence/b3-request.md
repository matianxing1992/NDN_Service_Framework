# B3 Prepared Request Evidence

**Spec**: `185-prepared-model-runtime`

**Batch**: `B3 (T005 -> T006)`

**Status**: `CLOSED_FOR_VALIDATION`

**Validation date**: `2026-09-13` (`America/Chicago`)

**Base**: `e81587ca3ef7512a98e15d8d96a43885610f383c`

**Final immutable composition snapshot**: `.codex-tmp/spec185-b3-composition-v6/`

**Final snapshot DIFF SHA-256**: `093b3dcf2e2ea77c982dd843f0830b470550bd458e1ecb11630ea014789ddd87`

**Final snapshot PATHS SHA-256**: `1f471fad074ed0179c92ff29d61f152faae97e052a151d8633a7f1d8002afe0f`

本记录是 B3 唯一批次结果，合并 T005/T006 的静态门、组合审查、共享
构建和 C++ 动态验收。快照包含新增的 `RuntimeTestAccess.hpp` 与请求
fixture；无关工作树修改未进入快照或本次提交。

## Scope and design binding

- **T005 / C-01, C-03, C-08**：`CD03`, `F03/F05/F16`, `FN03/FN08`,
  `FLOW03`, `PO03/PO08`。`PreparedModel` 将 verified immutable
  `PreparedModelPackage` 绑定到 Runtime-owned `NativeInferenceClient`，
  映射 inline/repository input、placement、stream/generation defaults，
  并为每次 request 分配独立 native ID；package、grant、candidate 和
  selection 不跨请求缓存。
- **T006 / C-01, C-03, C-05, C-06, C-08**：`CD03/CD04`, `F10-F12`,
  `FN04`, `FLOW04/FLOW07`, `PO04`。`RequestHandle` 保留 lease 与 native
  status/result/error；`result`/`events`/`observe`/`cancel` 遵守终态一次、
  deadline、历史/有界事件流和可退订 subscription 语义。
- Runtime client registry 使用 immutable atomic snapshot；notifier 在
  map/snapshot 发布前安装，Core drain 只读 snapshot，不在 Core worker 锁下
  取得 DI mutex。关闭时 borrowed Face、NAC-DKEY cleanup、operation runtime
  和 owner 的释放顺序保持可排空。

## Static review trace

审查 agent 使用本机官方
`/home/tianxing/.codex/skills/review-agent/SKILL.md`（skill SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7624e1a4f56292a4f3f92228`），
始终只读，不构建、不提交。T005/T006 的逐任务静态门覆盖了完整任务
diff、周边 caller、fixture、`tests/wscript` 和五 lane；最终受影响范围
复审 v5 返回 `T006_STATIC_PASS`，无 P0-P3。最终 B3 组合快照 v6 返回
`B3_COMPOSITION_PASS`，无 P0-P3，身份与本记录的 base/digest 完全一致。

| Lane | 实际范围与判定 |
| --- | --- |
| production entry/callers | `User::prepare` -> `User::request(PreparedModel)` -> `PreparedModel::requestInternal` -> `NativeInferenceClient::requestCooperative` -> Core operation；`Runtime::close/drain/drainAsync` 与多 client fixture。covered。 |
| implementation and wire | package projection、input/placement mapping、request ID/lease、deadline/cancel/terminal gate、event reader、Core ticket/drain、immutable snapshot/notifier、NAC-DKEY cleanup；无新增 wire schema，wire N/A 由 diff 证实。covered。 |
| test/harness/oracle | C++ `Spec185PreparedRequest` 12 cases、`Spec185ExtensionRegistry` 10 cases；独立 ONNX planning/semantic fixture、wrong digest/role、provider ACK/selection、cancel/deadline/slow-consumer/drain cases；`tests/wscript` 完整注册。covered。 |
| build/source closure | `NDNSF-DistributedInference` DI closure、Core `OperationRuntime`/`ServiceUser` consumers、`tests/wscript`；normal 与独立 ASan/UBSan target 均从对应 Waf tree 链接。covered。 |
| migration/evidence | C-07 handle/Runtime exposure、Core->DI dependency direction、唯一 B3 evidence、tasks 状态和 `docs/failure-log.md` 失败边界。covered。 |

**Batch growth decision**：B3 保持 `T005 -> T006` 两个成员。二者共享
PreparedModel/client/Core 请求出口和同一 C++ selector，但 T006 依赖 T005
静态完成；未吸收 B4 会话或 B5 Provider facade。组合门通过后才进行一次
批末构建/测试。

## Build and C++ runtime evidence

按 `C++ production -> C++ tests` 执行，使用系统 `/usr/bin/g++ 9.4.0`、
GNU `ld.bfd 2.34`、Boost 1.71（`/usr/include` +
`/usr/lib/x86_64-linux-gnu`）及 `-j4`；没有并发竞争构建。普通增量构建在
`build-spec185-b0c-normal` 完成，`.codex-tmp/spec185-b3/normal-build-v13.log`，
`rc=0`，`28.568s`。快速独立 ASan/UBSan 树
`build-spec185-b3-asan-ubsan-fast` 完成，`.codex-tmp/spec185-b3/asan-ubsan-fast-build-v4.log`，
`rc=0`，`38.277s`；运行使用
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1` 和
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，没有抑制。

最终 binary SHA-256：

- normal `spec185-prepared-request`: `b7cb37ae8431671e37d9d36d4006ac4df79faf6ee9b9af6fc62dde30ba71e015`
- normal `spec185-extension-registry`: `1ce325ad12bd3921cb290fc56d63f5bf2a84f8e2991bb12df4dd0f1c95130030`
- ASan/UBSan `spec185-prepared-request`: `babc7eb86abb9356e2b3d87f02e45f40d40a679af60c8dd49a03c98a6bb26986`
- ASan/UBSan `spec185-extension-registry`: `15b02af6dad51c87594ca078ecafcd0e352b06f502441cdecac61132d081431a`

normal `ldd` 的关键闭包实际解析到当前 normal tree 的
`libndn-service-framework.so.0.1.0`、`ndn-svs/build/libndn-svs.so.0.1.0`、
`nac-abe-integration-182/install-spec184-r4/lib/libnac-abe.so`，以及系统
Boost 1.71；没有使用另一棵 DI build tree。

## Dynamic gate card

每个 selector 顺序运行两次；第二轮不与其他 selector 并行。每次均
`rc=0` 且 `*** No errors detected`：

| Profile | Selector | Repetitions | Logs |
| --- | --- | --- | --- |
| normal | `Spec185PreparedRequest` | 12/12 x 2 | `normal-runs/prepared-request-v14.log`, `prepared-request-v16.log` |
| normal | `Spec185ExtensionRegistry` | 10/10 x 2 | `normal-runs/extension-registry-v3.log`, `extension-registry-v5.log` |
| ASan/UBSan + LSan | `Spec185PreparedRequest` | 12/12 x 2 | `asan-ubsan-fast-runs/prepared-request-r4.log`, `prepared-request-r6.log` |
| ASan/UBSan + LSan | `Spec185ExtensionRegistry` | 10/10 x 2 | `asan-ubsan-fast-runs/extension-registry-r3.log`, `extension-registry-r5.log` |

覆盖 inline/repository projection、independent request IDs、wrong role/digest、
unsupported input/generation、provider ACK/selection/response、local wait
timeout、cancel before/after terminal、event reader/observer、single/multiple
client drain、revoked hot-cache 和 fixture NAC-DKEY cleanup。ASan/UBSan 两轮
均未出现 `AddressSanitizer`、`LeakSanitizer`、UBSan 或 `DEADLYSIGNAL` 报告。

## Failure boundaries retained

先前失败没有被覆盖：normal prepared v10 的 drain deadline 在孤立 v2 通过后
仍保留为历史边界；ASan r1/r2/r3 的 deadline/LSan 报告保留在
`.codex-tmp/spec185-b3/asan-ubsan-fast-runs/`，并记录了 borrowed Face 的
NAC-DKEY `SegmentFetcher` 清理。对应修复是保持 Core close/drain 顺序并在
fixture 销毁前 pump 到 consumer ready；最终 r4/r6 已无泄漏。一次四 selector
并行重复还产生了 normal/ASan 资源干扰，原始日志和首边界见
`docs/failure-log.md`，没有计入资格；随后改用顺序隔离重复并通过。

## Retrospective and closure

| Gate | Result |
| --- | --- |
| static | T005/T006 逐任务门、受影响复审和 B3 组合门均 `PASS`，无控制性缺陷。 |
| compile-link | normal 与独立 ASan/UBSan 目标均 `rc=0`，binary/hash/ldd 闭包已记录。 |
| runtime-test | normal 与 ASan/UBSan 两个 C++ selector 各两次全通过；12+10 cases 每轮均无错误、无 LSan。 |
| unobserved | TSan 专项、B4-B9、Python binding、跨进程 B7 qualification、SIF/Tiger/MiniNDN 仍未运行；Spec184 外部资格不继承。 |

**Closure decision**: `CLOSED_FOR_VALIDATION` for B3; `T007/B4` is the next
dependency-satisfied batch. Spec185 overall remains `PLANNED` until all later
tasks and their native/Python/documentation gates close.
