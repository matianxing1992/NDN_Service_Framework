# Spec182 Request Chain Static Audit

**Date**: 2026-09-11 | **Status**: FINDINGS_OPEN / implementation PARTIAL
**Baseline**: `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a` on `Experimental`
**Dispatch authority**: [R12 audit-driven execution](../contracts/audit-driven-execution.md)

## Conclusion

当前已存在独立 C++ requester → Core → authority → planner → Provider → Response 的生产链，
以及 stream、两轮 conversation、受限 replacement 和 cleanup 接线。不能再将其描述为
“只有 helper，没有生产入口”，也无需从头重写这些能力。但主链接通不等于所有竞争分支闭合：
本轮确认 **4 个源码缺陷**，另有 **2 类完成度缺口**。因此不能宣布 Spec182 完成。

优先修复线程归属、会话状态同步和持久提交终态，再收敛维护中调用方，最后补资格矩阵。
旧 R11 的 PASS 只保留其原测试范围；本轮不把历史失败重判为当前失败，不重新执行产品测试。
新的调度以本报告为准，旧时间线和任务卡不再自动派发。

## Scope and Method

沿 `DI_NativeRequester` 的生产调用路径读到 Core 请求状态、规划、grant、Provider 装配执行、
流式终态与 conversation coordinator；交叉阅读 APPClient 路由、实际调用方和现有 C++ 测试。
这是当前快照审计，不是“最近一次提交引入回归”的断言，也不是逐行审完整个仓库或形式化证明。
Core 全部密码算法实现、每个模型后端内部数值算法、所有部署脚本不在此次逐路径深审范围。

只读阶段使用独立官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`，并应用项目
`skills/speckit-code-design/references/review-agent.md`。未委派子代理；审阅结束后单独写文档。
CodeGraph 先定位 requester/client/provider CLI，再按具体符号读取上下文、调用方和测试。
没有运行编译、单测、集成、MiniNDN、SIF 或 Tiger。以下源码行号固定于上述 baseline。

工作树原有 `docs/failure-log.md`（28 行新增）及
`tests/integration-tests/ndnsf-di-core-flow.t.cpp`（3 行新增）是其他工作单元；未将其归为本轮修复。
另有大量未跟踪构建/日志文件，本轮不接管、不提交。已阅读最新失败记录：错误 Waf tree、
MiniNDN trace 配对、replacement marker 与 Qwen fixture 路径是不同首失败边界，不能合并成协议失败。
Context Mode active health 返回 stale `tasks.md` source hash，故直接以 feature pointer、
Spec 文档、当前源码和持久证据为权威；不使用 timeline auto-memory 推断进度。

## Request Chain Coverage

路径缩写：`DI/` = `NDNSF-DistributedInference/cpp/ndnsf-di/`；行号均为源码定位。
“已接线”仅指读到生产调用，不代表所有负例已验证。

| Step | Production path / decision | Current assessment | Remaining closure |
| --- | --- | --- | --- |
| 1. 配置与身份 | `examples/DI_NativeRequester.cpp:190–252` 读取 catalog/runtime，拒绝 requester 配置 authority 私钥和明文工件密钥；注入独立 authority transport | 已接线；独立 authority 正负例有历史证据 | 保留身份隔离断言；不可把测试用 local issuer 构造器视为生产私钥共置 |
| 2. 建立请求与 deadline | `DI/NativeInferenceClient.cpp:1713–1900` 建 Operation、注册强引用、计时并排入 worker | 生命周期主路径存在 | F-02：跨线程读取/写入必须统一同步 |
| 3. Request 与 ACK closure | `DI/NativeInferenceClient.cpp:1100–1155`，`ndn-service-framework/ServiceUser.cpp:6882` 起 | ACK_CLOSED 后才允许 sealed plan；Core 核对 closure digest、deadline、plan/terminal role | 错候选如何影响整个候选集，保留严格拒绝现状，不擅自改为忽略 |
| 4. Admission 与 planning | `DI/NativeRequestPlanner.cpp:304–356` 及其后续 split/place/seal/projection 流程 | 验证 offer、形成 assignment、准备工件与每 role grant；不是接受 caller plan 为权威 | F-02：规划结果和 turn 的发布需与取消原子协调 |
| 5. Authority grant 往返 | `DI/NativeAuthenticatedGrantClient.cpp:69–150`；`DI/NativeArtifactPolicyAuthority.cpp` | 独立 authority 验证请求并签发；requester 验证返回后发布 grant | **F-01：worker 直接调用非线程封送的 Core 请求接口** |
| 6. Grant 完整性 | `DI/NativeGrantVerifier.cpp:490–510` 与 Provider verifier | 核对 request/provider/epoch/期限/recipient/digest/authority signature 与 canonical wire | 此处未发现“未验证答复直接用密钥”的路径；不等价于全密码学审计 |
| 7. Selection 提交 | `DI/NativeInferenceClient.cpp:1140–1155` → `ServiceUser::CommitCollaborationPlan` | 正常提交封送至 Face IO；Core 再核对冻结 ACK 集和 plan | 取消与提交交错应纳入 F-01/F-02 的生产 fixture，不只测 helper |
| 8. Provider admission 与装配 | `DI/NativeProviderHandler.cpp:1887–1935`；`examples/DI_NativeProviderExecutable.cpp:1695–1768` | authenticated Selection 后建立 GrantVerified runtime，验证绑定再按 projection 装配；不是启动时预装配冒充按需执行 | 保留 key expiry/role mismatch/worker 身份负例；全模型资格仍未完成 |
| 9. 依赖输入与执行 | `DI/NativeProviderHandler.cpp:1088–1127,2203` 起 → `NativeProviderRuntime` | 使用生产 async runner、dependency IO 和执行前密钥检查 | 需要维护真实后端/fixture 的覆盖；只读接线不证明多机或全模型数值一致 |
| 10. Unary Response | `DI/NativeInferenceClient.cpp:1158–1195` | committed attempt、终端 Provider、响应内容验证后产生唯一 handle 终态 | tiny ORT process 正例已有；完整模型/caller matrix 未闭合 |
| 11. Stream | `DI/NativeInferenceClient.cpp:300–480,1200–1275` | attempt fence、有序 token/prefix、final 一致性、大小/队列边界 | 当前定向 stream 正例不能覆盖超时/取消/重复/缺口全部交错 |
| 12. Continuation | planner conversation projection → `NativeProviderHandler.cpp:2073–2128` | parent checkpoint/role receipt 和本地 state reference 检查；找不到状态明确失败 | requester journal 不是 Provider KV 备份；重启失败证据不等于恢复成功 |
| 13. Receipt 与 COMMIT | `DI/NativeInferenceClient.cpp:685–996` | 收集绑定 receipt、发送 COMMIT、等 ACK，再由 durable gate 推进 journal | **F-03：持久提交与 handle 成功终态之间仍有竞争窗口** |
| 14. FINALIZE 与 cleanup | `DI/NativeConversationCoordinator.cpp:414–443`；client `markTerminal` | published 后不回滚；FINALIZE 异常被保留策略容忍；未提交 turn 会 abort/rollback | F-03 必须覆盖 finalize 阻塞/丢失时取消；持久恢复与 retention 资格另计 |
| 15. Replacement | `DI/NativeInferenceClient.cpp:1000–1100` | 受限的一次替换、attempt fencing、排除故障 Provider、保留 token prefix | 现有 attempt2 正例不证明 APPEND_DELTA 跨 Provider 状态迁移；缺 state 应保持明确失败，不能声称自动恢复 |
| 16. 导出与调用方 | requester `378–395`；APPClient `_native_client` 分支；五组 Python callers | C++ 路由与薄封装已存在，部分脚本需显式 native config | **F-04：checkpoint 文件权限窗口**；G-01：默认入口/模式退出未完成 |

## Findings

### F-01 [P1] Marshal authority requests onto the Core IO context

**位置**：`DI/NativeAuthenticatedGrantClient.cpp:83–86,116`；
`ndn-service-framework/ServiceUser.cpp:6119,6238`；`ServiceUser.hpp:250–258`。

authority acquisition 明确禁止在 IO 线程阻塞，因此由规划 worker 执行，但它直接调用
`RequestServiceTargeted()`。该 Core 方法并不自己 `postToIo`，而是直接访问和插入
`m_pendingCalls`，并继续处理请求；Face IO 的响应、超时和其他调用也访问这个容器。
回调结果上的 `Pending::mutex` 只保护等待结果，不能保护 Core 请求状态。
因此实际异步生产路径存在跨线程并发访问，可能导致未定义行为或请求状态损坏。

**修复**：把构造完成的 authority request 提交动作封送至现有 `ServiceUser::postToIo`，
在 IO 闭包内处理空 request ID 和异常；worker 仅等待同步结果。取消/过期在排队前后都检查，
明确已发送 authority request 的取消或有界收尾语义；不要在 worker 上新开 Face event loop。
**验收**：C++ fixture 用真实 Core IO owner 与独立 worker 调生产 transport，覆盖正常返回、
取消先于 IO dispatch、dispatch 异常、timeout 和晚回调；断言提交发生在线程 owner 上。

### F-02 [P1] Synchronize turn publication with terminal transitions

**位置**：`DI/NativeInferenceClient.cpp:1124–1135` 与 `516–556`；另见 `1891–1896`。

ACK worker 在锁外直接给 `operation->conversationTurn` 赋值（绑定 initial/replacement role map），
而 `markTerminal` 在取消/超时线程持 mutex 复制同一个包含字符串和容器的 optional。
只给读者加锁不能同步锁外写者；用户取消或 deadline 与 ACK planning 重叠即构成数据竞争。
deadline 错误构造对 `pending->attempt` 的锁外读取，也须纳入同一 mutable-state 清点。

**修复**：在锁内取得本地 turn/attempt 快照，锁外进行 coordinator/planner 工作，再在锁内
核对 Pending 和 attempt 后发布；终态抢先时必须撤销或释放本地未发布 ticket，避免只修 race
却留下 pending turn。明确 Operation 字段由 IO、worker 或 mutex 的哪一者保护。
**验收**：C++ barrier fixture 暂停在 bind 前后，分别并发 cancel、deadline、close 和 replacement；
断言单一终态、无残留 pending turn/重复 cleanup、旧 attempt 不发布。可用 TSAN 辅助，不能以
一次不崩溃代替确定性交错测试。

### F-03 [P1] Linearize durable conversation commit and handle outcome

**位置**：`DI/NativeInferenceClient.cpp:970–991,1265–1267,540–550`；
`DI/NativeConversationCoordinator.cpp:414–443`。

durable gate 持 Operation mutex 调 `publish()`，写 journal/替换 parent 并设
`conversationCommitted=true`，然后释放锁。FINALIZE 和 `commitTurn` 返回之后才调用
`markTerminal(Succeeded)`。在这段窗口内，cancel/deadline 仍能把 Pending 改成 Cancelled/Failed；
`conversationCommitted` 只阻止 abort，并不阻止失败终态。结果是持久 parent 已前进而 handle
报告失败，调用方无法从普通失败判断该 turn 是否已提交。此问题不意味着 journal 被回滚。

**修复**：定义并实现单一提交线性化点，使 durable publish 与成功结果的归属共同确定；
提交前取消赢则不得 publish，提交后取消不得把已提交请求降为普通失败。FINALIZE 的 best-effort
收尾仍可异步进行。若另行采用 outcome-unknown API，需要显式设计决策，不能静默改变现有语义。
**验收**：在真实 NativeInferenceClient 接线上把 FINALIZE 阻塞在 journal publish 之后，触发
cancel/deadline/close，检查 handle、checkpoint、journal、Provider promotion 一致。
`tests/unit-tests/di-native-conversation.t.cpp:436` 已测试 coordinator 的 published 不回滚，
但没有验证这个 client handle 的竞争窗口，因此不能用该用例关闭本 finding。

### F-04 [P2] Create transcript checkpoint exports securely and atomically

**位置**：`examples/DI_NativeRequester.cpp:385–395`。

导出 JSON 包含完整 transcript；当前先用 `ofstream` 创建/截断并写入，关闭后才 `chmod(0600)`，
且忽略 chmod 返回值。在常见 umask 022 下，新文件写入期间可被其他本地用户读取；已有宽权限
文件也在写入期间保持旧权限。中断或写入失败还可能破坏既有 checkpoint；普通打开会跟随 symlink。
此 finding 限于 CLI 导出路径，不指称内部 journal 使用同样不安全的实现。

**修复**：在目标目录创建排他、0600 的临时文件，检查全部写入/close 错误，按既有持久性要求
fsync 与原子 rename；明确 symlink 和已有目标策略，不在写完秘密后补权限。
**验收**：C++ 文件测试在 umask 022 下检查首次可见权限、写入失败保留旧 checkpoint、symlink
策略及成功导出再加载。此修复不应导致全仓库重编。

## Completion Gaps

### G-01 Maintained caller and mode closure

`examples/python/NDNSF-DistributedInference/llm_pipeline/user.py:4256–4268` 有 native config
分支，也保留 automatic planning 分支；`yolo_2x2/user.py:650,670,839` 同样为显式 native 模式。
APPClient 的 native forwarding 是条件分支（`app_sdk/client.py:1227,1276,2533`），并不使每个
旧调用点自动成为原生路径。YOLO native 分支还明确拒绝旧 lifecycle journaling 参数。

历史审计登记的“5 文件、16 调用点”可作迁移入口索引，**不能直接作为当前剩余工作数量**：
同一调用点可有多个模式，也已有 native 分支新增。应重新生成逐 caller/mode 清单，记录默认路由、
显式 native 路由、退出/兼容策略及各自 C++ oracle。保留兼容入口本身不是缺陷；若仍由 Python
拥有目标要求迁移的规划/状态逻辑，则不能关闭 T013/T015 和 no-Python。

### G-02 Qualification and recovery claims

当前已登记的实际证据包括 tiny unary、8-token stream、两轮 continuation、错误 parent 拒绝、
Provider 重启后的 stateMissing、一次 backup replacement 和 no-backup 拒绝。
见 [current cross-process chain](r11-b10-g13-current-cross-process-chain-20260910.md)。
这些证据证明相应边界，既不等于每个模型/调用方通过，也不等于 durable KV recovery 或多机资格。

全库 integration 历史失败、owner-probe 首失败、当前 selector PASS 必须分开维护；先修正确的
fixture/build identity/trace/marker，再运行对应 selector。逐项核对 I/PO 矩阵，不因一个缺项
重跑所有已通过用例。no-Python 需证明生产依赖闭包；Python 进程编排不能证明 runtime 内部无 Python。
跨机器/SIF/Tiger 仍按用户分工交由实验机执行，本机不得为关闭文档任务擅自启动这些实验。

## Non-findings and Boundaries

- 不把会话 COMMIT/ROLLBACK/FINALIZE 当成替代 NDNSF Request/ACK/Selection/Response 的新 Core
  协议；它们是 DI 跨请求状态的应用事务控制。问题在实现边界，不在额外控制消息的存在本身。
- 未发现需要恢复 requester 持 authority 私钥的理由；独立 authority 架构继续保留。
- 不要求无状态 backup 凭空恢复旧 Provider 的 KV；stateMissing 作为显式失败可以是正确结果。
  若目标要求透明迁移，须另行定义状态转移/重算契约和资格，不能把现有失败当成完成。
- 请求 ID 已有随机 namespace 与计数器；没有把“未做所有跨进程碰撞试验”写成确定性碰撞缺陷。
- FINALIZE 丢失有 retention 边界；本轮没有凭吞掉清理异常就断言 committed parent 应回滚。

## Review Gate and Next Work

| Coverage lane | Static review result |
| --- | --- |
| Production entry/callers | requester/authority/Provider CLI 与 APPClient 路由已对照；G-01 OPEN |
| Implementation/wire | Request→Response 和 conversation/replacement 分支已沿链检查；F-01–F-04 OPEN |
| Tests/harness/oracle | 对照 coordinator gate 测试和持久 process evidence；新竞争反例 NOT_RUN |
| Build/source closure | 使用已登记 build identity 作为证据边界；本轮不重编，不给依赖闭包 PASS |
| Migration/evidence | 旧 dispatch/状态标题存在过时信息；由 R12 单一调度表取代 |

**Closure decision**: OPEN_FOR_IMPLEMENTATION。报告完成不等于产品静态门 PASS。
**Miss retrospective**: F-01/F-02/F-03 是 `static` 可发现的线程/状态交错缺陷；F-04 是 `static`
可发现的文件生命周期缺陷。尚未运行反例的表现属 `unobserved`，不冒充 runtime-test failure。
历史错误构建树属于 build identity/`compile-link` 边界；trace/fixture/marker 属 harness/runtime
观测边界。新批次须先逐任务静态门、再批次组合审查、最后共享 C++ 构建和有针对性的验证。

下一步唯一 dispatch 为 **R12-A：修复 F-01/F-02 的线程与取消闭环**。完整批次、依赖和退出标准
见 [audit-driven execution](../contracts/audit-driven-execution.md)；不从旧 R11 时间线任选下一项。

## Documentation Validation

- `audit_speckit_structure.py --strict`：PASS；19 FR、11 SC、17 父任务，完成数仍为3。
- `verify-spec-kit-sync.py --require-entrypoints`：PASS，11/11。
- 新报告/契约本地文件链接存在，父任务 checkbox 与 baseline 一致；`git diff --check` PASS。
- 已核对产品路径无本轮 diff；原有 failure-log 和 integration fixture 改动保持原边界。
- authority index 刷新后 Context Mode project/active health 均 `ok=true`；初次 stale hash
  已修复。本轮审计结论仍来自仓库源码与持久文档，不由索引命中产生。
- 产品运行验证 NOT_RUN；本表只验收文档单元，F-01–F-04 仍 OPEN。
- 首次普通本地 commit 被 `.git/hooks/pre-commit` 的全索引开发助手引用检查拦截，未产生提交；
  核对该 hook 后使用其既有 `NDNSF_LOCAL_CHECKPOINT=1` 本地 checkpoint 通道重试。
  这不是产品或文档结构测试失败，没有移除 hook、删除历史引用或扩大暂存范围。
