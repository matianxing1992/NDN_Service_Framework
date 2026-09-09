# R10-B65 Large-Scale Static Audit

## Scope and Baseline

本轮审计以本地 `Experimental` 当前提交 `3608f204` 为源码基线，覆盖 Spec182
native requester/Core/Provider、maintained Python callers、pybind facade、Provider
host 生命周期、Waf source closure、测试注册和资格证据。审计为只读工作单元：没有修改
产品源码、skill、构建树或既有原始运行记录。工作区中其他会话留下的
`native-dependency-design.md` 与 `native-generation-design.md` 修改保持原样。

任务表当前为 17 个父任务，其中只有 T001--T003 完整关闭；40 个 execution cards
分布为 16 `DONE`、23 `PARTIAL`、1 `NOT_STARTED`。这个分布反映工作记录，不是
生产能力完成率。

## Method and Results

| Check | Scope and command | Result |
| --- | --- | --- |
| Spec structure | `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | `PASS`; 19 FR、11 SC、5 user stories、17 parent tasks、40 cards、1101 links；`runtime_tests=NOT_RUN`、`product_static_review=NOT_RUN` |
| Repository graph | `codegraph status .` plus symbol/caller inspection | index current；8,175 files、276,283 nodes、674,478 edges |
| Python syntax | AST parse of maintained package, examples, `pythonWrapper` and `tests/python` | 626 files，0 syntax errors |
| C++ static scan | `cppcheck --enable=warning,performance,portability --inconclusive --inline-suppr --language=c++ --std=c++17 NDNSF-DistributedInference/cpp/ndnsf-di` | exit 0；报告了 1 个已核对的 iterator lifetime 误报、1 个冗余条件和 1 个 pass-by-value 风格项；未把它们冒充为新的资格 PASS |
| Caller inventory | maintained examples and `app_sdk` route search | `yolo_split`、`llama_server`、`pytorch_eager_2x2` 仍有 `distributed_inference()`；`yolo_2x2`/Qwen native route 仍需显式配置或分支选择 |
| Skill synchronization | `verify-spec-kit-sync.py --require-entrypoints --require-personal --json` | `PASS`; 11 repo entrypoints 与 personal `speckit-code-design` copy 一致；本轮没有修改 skill |

## Coverage Matrix

| Lane | Static evidence covered | Boundary still open |
| --- | --- | --- |
| Production entry/callers | `APPClient.request_native_reference`、Qwen helper、YOLO native branch、Provider executable | canonical `request()`/`request_streaming()` 仍 planner-first；多个 maintained caller 仍走 Python durable/legacy route；没有全部默认 native 的证据 |
| Implementation/wire | `NativeInferenceClient::dispatchOperation`、`beginCoreRequest`、`NativeRequestPlanner`、Provider final schema、pybind DTO | direct C++ contract 的 generation-mode 校验不完整；application `wire_request_id` 到 native owner request identity 的映射仍未冻结 |
| Provider/worker | `NativeInferenceProvider::serve/stop`、`NativeProviderHandler`、ONNX worker location and hash checks | 没有当前源码/证据证明 maintained native-config Qwen/YOLO 已经完成真实 Core→Provider worker/cross-process 传输 |
| Test/harness/oracle | 既有 C++/Python selectors、626-file AST、Waf lists、R10-B1--B64 evidence | 静态与 in-process selectors 不能替代真实 cross-process、MiniNDN、no-Python、数值 parity 和 I02--I08/PO matrix |
| Migration/qualification | `tasks.md` P1--P7、344-entry manifest、T013--T017 owner | manifest 是 routing inventory；legacy zero-use、T015 convergence、T016 qualification、T017 handoff 未关闭 |

## Findings

### F-01 `HIGH` — Maintained default routes still enter the Python planner

`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:628-657`
的 `APPClient.request()` 在没有 `AutomaticPlanningCoordinator` 时直接失败，并把请求
交给 `_automatic_planner`；`:798-909` 的 streaming/generate 也保持同样的 planner-first
边界。维护中的 `yolo_split/user.py:75-81`、`llama_server/user.py:95-101` 和
`pytorch_eager_2x2/user.py:49-65` 仍调用 `distributed_inference()`；
`yolo_2x2/user.py:702` 的 native reference 只在显式 native 配置分支使用。

这说明原生组件已经存在，但 Spec 要求的“所有 maintained callers 默认由同一 native
owner 组合、提交和回收”尚未实现。这个问题属于 T013/T016 前置条件，不能由 native
binding 单测或 compatibility manifest 关闭。

### F-02 `MEDIUM` — Public direct C++ contract bypasses generation-mode validation

`NativeInferenceClient` 的公开 contract 构造函数
(`NativeInferenceClient.cpp:1529-1543`) 只检查 service、task、preparation 和
admission，没有检查 `generationMode`。`encodeNativeRequestEnvelope`
(`NativeRequestEnvelope.cpp:118-128`) 也只拒绝空值，随后在 `:180-184` 原样写入
`task.generation_mode`。相比之下，JSON runtime parser
(`NativeRequestPlanner.cpp:217-220`) 才限制值为 `TOKEN_DIAGNOSTIC` 或
`TOKEN_STREAMING`。

因此一个直接使用 public C++ contract 的调用方可以构造 `"UNSUPPORTED"` 并产生格式
看似合法但语义不受支持的 envelope。现有单测只覆盖 JSON runtime 的非法 mode，未覆盖
direct contract constructor。这是跨入口契约不一致，应在 T010/T012 的下一批统一为
同一 fail-closed 校验。

### F-03 `MEDIUM` — Long-lived native client retains expired operation entries

`NativeInferenceClient.hpp:216` 用 `std::vector<std::weak_ptr<...>> m_operations` 保存
提交记录，`NativeInferenceClient.cpp:1729` 每次 request 都追加；只有 `close()` 的
`:1770-1780` 清空。已结束 operation 的 weak entry 不保持对象存活，但会在线性增长的
vector 中长期占据容量，并在每次 close/遍历时增加开销。当前没有周期性 compact 或按
terminal 状态移除的路径。

这是可由静态审计确定的长期资源问题，不等同于立即 UAF；应补一个有界 retention 规则
和长生命周期回归，而不是借现有 cancel/close selector 宣称已经解决。

### F-04 `MEDIUM` — Provider host initialization is not transactional on runtime failure

`NativeInferenceProvider::serve` 在 `NativeInferenceProvider.cpp:257-280` 首次调用时
先创建 `HostState`、注册固定 lease entry 并写入 `m_host`，直到 `:341-347` 才调用
`makeNativeProviderCollaborationRuntime` 和外部 `runtimeObserver`。这些步骤抛出异常时
没有对应的 `m_host.reset()`/固定 entry 回滚；只有固定 entry 自身注册失败的 `catch`
才清理 `m_host`。

结果是一次失败的首次 serve 可以留下没有 target 的固定 lease 和已经锁定的 provider
identity/worker range。若失败配置使用空或错误的 immutable identity，后续以正确配置
重试会在 `:287-301` 被判定为 host conflict。现有 host lifecycle tests 覆盖 active
duplicate、close/re-serve 和 stop，但没有覆盖“首次 runtime assembly 失败后重新 serve”
的恢复边界。

### F-05 `OPEN DESIGN/EVIDENCE GAP` — Application/native request identity mapping remains unproven

native owner 在 `NativeInferenceClient.cpp:1658-1663` 分配
`/NDNSF/DI/REQUEST/N`；maintained Qwen caller 仍保存自己的 `wire_request_id`，而
`APPClient.request_native_reference` 没有把该值作为 native owner identity 参数传入。
现有 Core transport 校验能证明 native 内部 request/attempt 绑定，但没有一条 Qwen caller
边界的观察证明 application request、native handle 和最终 response 使用的是同一 identity
契约。这个选择可以是“native owner 是唯一权威”，也可以扩展显式映射，但在 T015/T016
决定并观察前不能把 caller 标成 identity-qualified。

### F-06 `OPEN PRODUCTION GATE` — Real worker/cross-process and no-Python closure is absent

当前源码包含 `NativeInferenceClient` 的完整 runtime 分支、`DI_NativeProviderExecutable`
的 Provider host 和 post-Selection ONNX worker location/hash 校验；已有 R10-B33/R10-B37
等证据覆盖单进程或 in-process Core/Provider selector。然而 maintained native-config
Qwen/YOLO 没有当前证据显示真实 requester → Core → Provider worker/cross-process stream
已经成功完成；conversation owner、alternate-provider recovery、legacy zero-use、
MiniNDN/no-Python 也仍在 T013/T014/T016。

这是审计确认的资格缺口，不是静态工具能够单独证明“运行失败”的缺陷。下一步必须用
真实进程边界和失败/取消反例补证据，不能把已有组件数量当成闭合。

### D-01 `MEDIUM` — Normative document status still lags current source/task state

当前 `contracts/runtime-boundaries.md:3-10` 仍写着 `DRAFT / BLOCK for implementation`
及“all Native* new interfaces planned”，`spec.md:277-282` 仍写 T001 未完成；而
`tasks.md` 已记录 T001--T003 DONE、R10-B1--B64 的 native implementation batches。
这些句子可能是历史设计基线，但没有明确标注为 historical，容易让执行者把已实现边界
当作未开始，或把设计关闭误读为产品完成。T015 前应统一当前状态与历史设计状态的
表达；本轮只记录，不修改文档。

## Static Review Benefit and Limits

静态审计确实有收益。此前已通过静态读码提前发现 Provider source closure、锁顺序与
scope 清理、replacement 状态、token prefix、final `tokenIds` 映射和 manifest source
identity 问题；本轮又发现 F-02 的 direct C++ mode bypass、F-03 的 retention 设计、
F-04 的 Provider host 首次失败回滚缺口，并确认 F-01/F-05/F-06 仍未闭合。

静态检查不能证明 NFD/MiniNDN forwarding、跨进程 worker 的真实数据流、授权传播、Qwen
数值 parity、legacy zero-use 或 no-Python qualification。`cppcheck` 的 exit 0 也不代表
这些生产边界通过；本轮 `validate_design.py` 和 skill synchronization PASS 只说明文档
结构及 skill 入口一致。

## Distance and Closure Decision

当前已经完成大量 native value contracts、ONNX/tokenizer、plan/placement、grant/admission、
Provider host 和 bounded in-process requester/Core/Provider selectors，但距离“所有改动
真正完成并可资格验收”仍然较远。剩余工作是有顺序约束的后半段：

1. 修复或冻结 F-02、F-03、F-04、F-05，并为每项增加对应 selector/证据。
2. 让一个 valid native-config Qwen 或 YOLO 请求真实经过 requester → Core → Provider
   worker/cross-process，并观察 terminal、cancel/deadline 和 response identity。
3. 完成 maintained caller 默认迁移，再证明旧 runtime zero-use/retirement。
4. 执行 T014 隔离反例、T015 convergence、T016 unit→integration→MiniNDN/no-Python，
   最后才进入 T017 handoff。

不提供可靠百分比：3/17 是父任务完整关闭数，16/40 是 leaf card 状态分布，二者都不能
代表生产能力比例。当前门结论为 `OPEN_FOR_NEXT_BATCH`；T010、T011、T012、T013、T014、
T015、T016、T017 仍不能提升为完整 DONE/qualification PASS。

## Validation Boundary

本记录仅包含静态审计和现有证据复核；没有新的 native build、NFD/MiniNDN/Tiger 运行或
源码/skill 修改。审计后的任务状态仍需保守保持父任务未关闭；该证据可作为 T015 的输入，
不能替代 T016 的运行资格。
