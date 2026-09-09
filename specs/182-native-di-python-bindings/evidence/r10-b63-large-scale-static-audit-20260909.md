# R10-B63 Large-Scale Static Audit

## Scope and Baseline

本轮审计针对 Spec182 当前源码、维护中的 Python caller、C++ native requester/Core/Provider 接线、pybind 边界、Waf source closure、测试注册和既有资格证据。基线为本地 `Experimental` 提交 `d5ec19964d6e667ef21eb2dbb982806d1712d7d9`。审计只读，不修改源码、skill、构建树或既有原始运行记录；两个其他会话留下的设计文档修改未纳入本批次。

本轮没有把任务卡数量当作完成度。当前 40 个 leaf task 中 16 个为 `DONE`、23 个为 `PARTIAL`、1 个为 `NOT_STARTED`；这只是工作分布，不能替代生产请求、跨进程和资格出口。

## Method and Results

| Check | Scope and command | Result |
| --- | --- | --- |
| Structure | `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | `PASS`; 19 FR、11 SC、5 user stories、17 parent tasks，40 execution cards，links 1099，tasks complete 3 |
| Repository graph | `codegraph status .` and targeted requester/Core/Provider exploration | index up to date，约 8,175 files、276k nodes、674k edges；实际源码抽样与调用路径已复核 |
| Python syntax | AST parse of maintained `NDNSF-DistributedInference`、examples and tests roots，排除 build copies | 636 files，0 syntax errors |
| C++ static scan | `cppcheck --enable=warning,performance,portability --inline-suppr --suppress=missingIncludeSystem --quiet NDNSF-DistributedInference/cpp tests/unit-tests` | exit 0；213 lines，其中大量 Boost macro/parser notices；可行动候选为 1 个冗余条件、1 个 pass-by-value、1 个初始化列表建议；`NativeEpochCoordinator.cpp:713` 的 dangling-lifetime 是 iterator 在返回前消费完毕的误报，未计为产品缺陷 |
| Source/build closure | `examples/wscript` source lists、native binding exports、existing R10-B49/B50/B51/B52 evidence | 当前 Provider source closure 与 CLI/link boundaries 有历史修复证据；未把它们误判为 runtime qualification |
| Caller inventory | maintained package and `examples/python/NDNSF-DistributedInference` search for native route, planner and distributed adapters | canonical APPClient 仍保留 planner-first defaults；Qwen/YOLO native routes 是显式配置或分支，不是全体默认路径 |
| Spec Kit synchronization | `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal --json` | `PASS`; 11 local entrypoints and personal shared skill checked；本轮没有修改 skill |

## Five-Lane Coverage Matrix

| Lane | Covered | Boundary still open |
| --- | --- | --- |
| Production entry/callers | `APPClient.request_native_reference`、Qwen `_native_qwen_request`/full-generation、YOLO native branch、`DI_NativeRequester` and Provider executable | `APPClient.request`/`request_streaming` 仍要求 `AutomaticPlanningCoordinator`；`yolo_split`、`llama_server`、`pytorch_eager_2x2` 仍调用 `distributed_inference`；YOLO native branch 需显式 `--native-requester-config` |
| Implementation/wire | `NativeInferenceClient::request`/`dispatchOperation`/`beginCoreRequest`、`NativeRequestPlanner` stream contract、Provider final schema、pybind DTO/observer | native Core allocates its own request ID，caller-supplied `wire_request_id` 与 native handle identity 的映射没有形成一条可由 Qwen native branch 强制验证的契约 |
| Test/harness/oracle | 249 `Spec182*` C++ unit cases、现有 in-process Core/Provider selectors、20 native Qwen + 8 legacy exclusion Python cases、PO-001/I01 owner observations | 没有 valid native-config Qwen requester → Core → Provider cross-process stream；conversation owner、replacement/recovery、I02–I08/PO-002–PO-014 和 no-Python maintained caller 仍未观察 |
| Build/source closure | Waf `di_native_session_sources`/`di_native_collaboration_sources`/ONNX closure、R10-B50 Provider link、R10-B51 check-only、R10-B52 requester CLI | `CertificateBootstrap.cpp` 在 Provider source list 中重复出现（低风险清理项）；历史 link closure 通过不等于当前 caller 或 network request 通过 |
| Migration/evidence | 344-entry compatibility manifest、tasks.md 的 P1–P7 production order、R10-B59–B62 evidence | manifest 是 routing inventory，不是 semantic zero-use；T013-B、T015-A、T016-A 仍需真实结果，T017-A 尚未开始 |

## Findings

### F-01 `HIGH` — Default maintained routes still enter the Python planner

`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py:620-649` 的 `request()` 和 `:763-890` 的 `request_streaming()` 在没有 `AutomaticPlanningCoordinator` 时直接失败，并将请求交给 `_automatic_planner`。`distributed_inference()` 在 `:1011-1080` 仍是 Python durable adapter。维护中的 `yolo_split/user.py:75-81`、`llama_server/user.py:95-101` 和 `pytorch_eager_2x2/user.py` 仍走该兼容入口；`yolo_2x2/user.py:839-848` 只有显式 native 配置才选择 native branch。

这不是“组件还没测试”的问题，而是 T013-A/T013-B/T013-F 的真实迁移缺口：当前不能声称所有 maintained callers 的默认请求已经由 C++ owner 组合、提交和回收。旧路径继续保留是当前安全策略，但必须在 P3/P4 产生真实 native 结果后再做 retirement。

### F-02 `HIGH` — Native-config Qwen accepts a diagnostic contract that its helper cannot execute

`client.py:398-402` 将缺省 `generation_mode` 设为 `TOKEN_DIAGNOSTIC`，只有 `TOKEN_STREAMING` 才要求 tokenizer digest；该值随后写入 native runtime contract (`:450-460`)。但 Qwen helper `llm_pipeline/user.py:115-160` 无条件构造 `TOKEN_STREAMING` output/generation DTO，C++ `NativeRequestPlanner.cpp:364-367` 又要求 generation 必须同时存在 stream 且 mode 为 `TOKEN_STREAMING`。

因此，能通过 Python config parsing 的 diagnostic native config，在实际 Qwen native request 时会晚到 planner boundary 才被拒绝，且没有针对配置与 Qwen helper 的早期一致性门。这是静态可证明的跨层契约冲突，应在 T013-D/T013-E/T010 的下一批中统一为早拒或真正实现 diagnostic route。

### F-03 `MEDIUM` — Qwen native observer can fail open on a JSON value that is not a mapping

`llm_pipeline/user.py:2874-2895` 对 raw payload 做 JSON decode 后直接调用 `token_event.get("schema")`，没有确认 decode 结果是 mapping。合法 JSON scalar/list 会触发 `AttributeError`；`NativeInferenceClient.cpp:550-557` 明确隔离并吞掉 observer exception，因此 `native_stream_errors` 不会记录该异常，后续 terminal event 仍可能使 full-generation branch 继续完成。

这会把 malformed non-terminal event 转成“没有观察到错误”的状态，属于 caller-edge validation 的 fail-open。需要增加 mapping 类型门和回归用例；现有 20/20 Python focused cases 没有覆盖该输入形状。

### F-04 `MEDIUM` — Opt-in diagnostic token loop reads a field absent from native final wire schema

CLI 仍公开 `--diagnostic-token-loop` (`llm_pipeline/user.py:3787-3790`)。当该 flag 与 native config 同时使用时，`token_step` (`:3035-3066`) 调用 `_native_qwen_request()`，随后强制读取 `response["topToken"]`。native helper 固定使用 `TOKEN_STREAMING`，而当前 C++ final contract 是 `NDNSF-DI-FINAL-V1` + `tokenIds`；full branch 才在 `:2920-2925` 将 `tokenIds` 规范化为 `generatedTokenIds`。因此该可达组合会在 caller 处产生字段错误，而 CLI 校验没有提前拒绝它。

该路径是显式诊断选项，不代表默认 full-generation 已走此分支；但它仍是一个可静态复现的未覆盖组合，应在 native route 完成前明确禁用或实现独立的 diagnostic response adapter。

### F-05 `MEDIUM` — Long-lived native clients retain expired operation weak entries

`NativeInferenceClient.hpp:216` 使用 `std::vector<std::weak_ptr<...>> m_operations`，`NativeInferenceClient.cpp:1729` 每次提交追加，只有 `close()` (`:1770-1780`) 才清空。已结束 operation 的 weak entries 在长生命周期 client 中不会被周期性移除，持续请求会使该表线性增长。weak pointer 不保持 operation 存活，因此不是立即的 ownership/UAF 问题，但属于可由静态审计发现的长期内存/遍历开销问题。

### F-06 `OPEN DESIGN/EVIDENCE GAP` — Native response identity mapping is not proven at the Qwen caller edge

Qwen caller 保存 `wire_request_id` (`llm_pipeline/user.py:2862-2866`)，但 `APPClient.request_native_reference` (`client.py:562-618`) 没有 request-id 参数；`NativeInferenceClient.cpp:1658-1663` 总是由 native owner 分配 `/NDNSF/DI/REQUEST/...`。Qwen native full branch (`user.py:2897-2927`) 校验 schema/tokenIds，却不把结果 handle 的 native request ID 与 `wire_request_id` 或 final payload identity 绑定。C++ Core 对 transport identity 有完整校验，但当前 caller 没有一条可观察的 application-to-native mapping 证明。

这项先归为契约/证据缺口，而不是直接断言安全漏洞：设计可以选择 native owner identity，也可以扩展 request option；在选择前，T015/T016 不应把 native-config Qwen caller 当作 identity-qualified。

## Static Review Benefit and Blind Spots

静态审查已经产生真实收益。历史记录中的 R10-B50 在运行前发现 Provider target 漏掉 18 个 native translation units；R4-B4/CC-3B 在编译前发现锁顺序、scope-key 提前清理、replacement 状态泄漏和 token prefix 半提交；R10-B61 发现 native final wire 的 `tokenIds` 与 Python full-generation 需要的 `generatedTokenIds` 不一致；R10-B62 发现 compatibility manifest 的 `sourceCommit` 过期。这些问题不能由“单元测试数量增加”推断出来。

本轮又发现 F-01 至 F-05 五个当前源码问题，并确认 F-06 仍缺证据。与此同时，静态门无法证明 NFD/MiniNDN forwarding、真实 Provider worker/cross-process transport、凭据在实际网络中的授权传播、真实 Qwen numeric parity、legacy zero-use 或 T016 qualification。PO-001/I01、249 个 C++ cases 和 Python compatibility PASS 只能关闭各自边界。

## Distance and Closure Decision

当前距离“所有改动完成并具备 Spec182 资格”仍然较远，但已经不是从零开始：native plan/ONNX/tokenizer、Provider host/link/readiness、in-process unary/stream selector、binding DTO 和 collector/I01/PO-001 局部出口已有可复核成果。未闭合的主要是生产链后半段：

1. P1 还需 valid native-config requester 进入真实 Core/Provider unary/stream，并保留 cancel/deadline/terminal 结果；
2. P2 需跨 Provider worker/进程闭合 continuation、receipt/control/commit、replacement/recovery；
3. P3/P4 需让 maintained YOLO/Qwen caller 产生真实 native 结果并配置 conversation owner；
4. P5 才能证明 default import graph 与 maintained caller 对旧 runtime 的零使用；
5. P6/P7 才能完成 I01–I08、PO matrix、T015 convergence、T016 qualification 和 T017 handoff。

`Closure decision: OPEN_FOR_NEXT_BATCH`。下一批的稳定出口应先处理 F-02/F-03/F-04、冻结 F-06 的 request identity contract，再执行真实 cross-process Qwen/YOLO 请求；在这些结果出现前，不升级 T013-B、T015-A、T016-A，也不生成 T017 handoff。

## Validation Boundary

本记录只包含静态审计与现有证据复核；没有产品源码修复、没有新的 native build、没有新的 NFD/MiniNDN 运行。既有工作区的 `native-dependency-design.md`、`native-generation-design.md` 修改及大量未跟踪构建/运行产物保持原样。
