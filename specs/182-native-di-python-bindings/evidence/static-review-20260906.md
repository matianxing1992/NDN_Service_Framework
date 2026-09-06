# Pre-MiniNDN Static Review

**Status**: PASS

状态仅表示本轮代码与设计静态审查、修复及对应 C++ 回归完成，不表示整体合并、Python 或 MiniNDN 验收完成。测试后独立复审核对 P03 真实 fetch/handler 队列撤销、P04 四种析构时机、两个真实 deadline 失败状态及 D2h212 五角色/唯一最终响应，未发现剩余确定性问题。D2h 使用确定性模型 runner；不得外推为真实 ONNX 模型资格或完整授权资格。

## Current Verified Checkpoint

2026-09-06 static R2：全部 C++ 回归通过。Full unit **759/759 PASS、63846 assertions**（60.910 s）；GDB full integration **154/154 PASS、3634 assertions**（209.203 s、exit0）。定向 expired/D2h121/D2h212 **3/3 PASS、60 assertions**（4.405 s）。构建 R3 全目标 **PASS**（391.882 s），Core Python 扩展重建 **PASS**（119.945 s），实际联合 native closure **PASS**（0.733 s）。Repo 扩展沿用 static R1 的同一最终 header ABI 构建；本轮只再修改 Core `.cpp` 和测试，实际加载 Core 新 SHA 已复核。

原始目录为 `.codex-tmp/merge-20260906/{build-static-review-r3,deadline-d2h-static-r2,unit-static-r2,integration-static-r2,native-build-static-r2,runtime-closure-static-r2}/`。当前 Python compatibility profile 正在重新执行；MiniNDN NOT_RUN。下方早期 checkpoint 是过程证据，不代表当前仍有这两项 C++ 失败。

## Integration Regression Repair R2

完整 static R1 的两个失败已定位，修复待重新编译验证：

- `makeCollaborationWorkFence`：`completeCollaborationRoleOnEventLoop` 会清理 ACK/token horizon，不能据此取消同请求其他角色。入队时仍捕获原始 horizon 作为不可延长的 deadline 上限；后续检查保留停止标志、时间、ControllerVersion、授权及 service binding，不再要求临时 ACK 表项存在。D2h121/212 的完整角色计数和数值 oracle 保留。
- `Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState`：过期请求现在在 Core 调用 native handler 前被拒绝。保存 `CommitCollaborationPlan` 后公开 snapshot 的真实 Selection digest，再读取各 Provider 的终态；要求两个 Core Failed、`REQUEST_DEADLINE`、`runningAtUs=0`、native failure/completion 均为零，仍要求无事件及无 decode cache 残留。没有模拟状态或删除失败证明。Core 两处 stream deadline 拒绝输出明确错误原因。

原始失败 `.codex-tmp/merge-20260906/integration-static-r1/` 和 D2h trace `.codex-tmp/merge-20260906/d2h-fence-trace-r1/` 保留；MiniNDN 仍为 NOT_RUN。

## Scope

提交范围检查：本轮修复的 unstaged `git diff --check` PASS。整合全部历史源码/文档后 cached check 报129条空白提示、涉及69文件；逐条与原主工作区或 `MERGE_HEAD` 内容核对，全部为继承的 Markdown 行尾空格/EOF空行，未发现新增未归属问题。保留既有/冻结证据字节，不以格式化重写旧 hashes；另检查冲突标记和本轮代码差异。

本次审查先于 MiniNDN，覆盖合并后的实际 staged/unstaged 源码；本地基线 `d4a5e39ce5b4a023f6e55d2440c60aa998983f8f`，远端 `4391af81cd24ff5510aa52b48ab9cec0fdec1ebb`。`git diff <parent> -- <paths>` 包含未提交合并修复；`...HEAD` 会漏掉本轮工作。两个审查轴分别保留，不以局部检查宣称全树 PASS。

## Standards

- S01 / MEDIUM / FIXED: Repo wrapper 接受 `NDNSF_LIBRARY_DIR=':'`，静默使用系统 Core。拒绝无有效目录的显式值；两份 wrapper 负例与既有正例 **14 PASS**，原始 `.codex-tmp/merge-20260906/setup-static-review-r1/`。依据 AGENTS 的匹配依赖闭包要求。
- S02 / FIXED: fetch worker 出队后才计算30秒预算；排队等待必须消耗原预算，不能延长原始请求授权窗口。
- S03 / FIXED: assignment fetch worker 的异常只被 pool 日志捕获，无法完成 `pending/onReady`；应转换为失败完成，保留恰好一次回调。

## Spec

- P01 / HIGH / FIXED: Controller 大写 SHA256 与 User/Provider 小写 certificate digest 逐字比较，证书撤销未匹配。依据 Spec179 `spec.md:110,221`。设计：共享有效 SHA256 十六进制摘要比较，兼容既有大写 signed/persisted wire；新输出规范化，错误 digest/identity 仍拒绝。涉及 `PolicyStatus.hpp/.cpp`、`RevocationState.cpp`、`ServiceController.cpp`；复用真实双角色撤销测试并补旧状态回归。
- P02 / HIGH / FIXED: User identity route 重试捕获局部 `onFail` 引用，构造结束后发生 UAF。依据 Spec181 FR-002 grant 的跨节点精确取数要求。设计：User 拥有可取消 registration handle，失败回调和 timer 使用弱生命周期引用，保持8次/250ms预算；析构先取消，验证失败重试及销毁后继续 dispatch。涉及 `ServiceUser.hpp/.cpp` 和 focused unit。
- P03 / HIGH / FIXED: Controller 更新已清除 collaboration key 状态，旧排队回调仍可能通过 `operator[]` 重建缓存并调用 handler。依据 Spec179 下一受保护转换拒绝旧版本的要求。设计：Provider 在 fetch 出队、Face 安装结果和 handler 出队检查捕获 version/request lifetime；失效结果不得复活缓存。deadline 以入队时预算及已有更早截止时间为界，cleanup grace 不冒充严格执行 deadline。涉及 `ServiceProvider.hpp/.cpp` 和延迟撤销回归。
- Compact AAD 局部关系检查通过：inner 认证字段恢复 capability 原值；outer manifest 与实际取数仍受 Selection edge deadline 限制。既有 D2h oracle **2/2 PASS**。

## Repair Symbol Contract

| File / symbol | Parameters / state | Required behavior |
| --- | --- | --- |
| `PolicyStatus.hpp/.cpp::sameCertificateDigest(const std::string&, const std::string&) -> bool` | 两个 certificate identity 字符串，不接收权限或网络状态 | 完整合法 `sha256:` +64hex 的字节身份忽略hex大小写；旧 opaque 标识仍精确比较。仅比较摘要，identity/service 范围继续由调用方检查；不改写 signed wire。Controller 新 certificate digest 输出小写。 |
| `ServiceProvider::CollaborationWorkFence` | `steady_clock::time_point deadline`; `std::function<bool()> current` | deadline 在入队时固定；current 捕获原 ControllerVersion、已有 cleanup horizon、service/request 关联和共享停止标志。只读检查，不创建授权或请求状态。 |
| `ServiceProvider::makeCollaborationWorkFence(requesterName, requestId, serviceName, optional<ControllerVersion>)` | 三个 `ndn::Name` 明确请求作用域；version 来自原请求或同步准入时已接受状态 | 使用现存 cleanup horizon 和本地 fetch 预算的较早者；撤销、版本变化、清理、超时或析构均使工作失效。cleanup grace 不等于全局执行 deadline。 |
| `ServiceProvider::fetchAndDecryptLargeDataUntil(const ndn::Name&, const std::string&, steady_clock::time_point, std::function<bool()> = {}) -> LargeDataFetchResult` | exact encrypted Data name、service URI、入队截止时间、内部权威检查 | 公开双参数 fetch 保留；新增 private helper 将排队和取数计入同一预算，等待/解包/完成均检查取消；错误完成，不复活缓存。 |
| `ServiceProvider::m_fetchStopping` | 已有共享 `atomic<bool>`，析构先置 true | 除 fetch 外，同时保护 handler 的失败和成功回调；worker 与 Face callback 均在访问 owner 前检查。 |
| `ServiceUser::registerIdentityPrefixWithRetry(size_t attempts = 0)` | 已失败次数；初值0，最多8次，250ms间隔 | 唯一调用入口为生产构造器；测试通过派生 LocalMock 调用真实 protected helper。失败回调和 timer 不引用栈对象。 |
| `ServiceUser::m_identityRegistration` | `shared_ptr<ndn::ScopedRegisteredPrefixHandle>`；未启动为空，User独占生命周期 | 回调只持 weak owner；析构首先 reset，取消后续重试并注销成功路由。仍遵守 Face 线程上的对象销毁约定。 |

## Additional Validation Diagnosis

Controller policy fixture 的 DummyClientFace 无法完成新增的独立真实 probe round trip；私有 NFD 定向 R1 仍在 PUBPARAMS timeout。显式 test-access 仅注册真实 policy handlers，继续检验 status/权限/撤销行为；不伪造 readiness。完整 `start()` 保留13项 standalone 和真实 NFD 边界验证。涉及 `tests/integration-tests/controller-revocation-flow.t.cpp`；完整 integration 待重跑。

Python 联合测试首异常：CLI 测试污染 `sys.path`，`packaging/ndnsf-di-container/lib/profile.py` 遮蔽标准库，`cProfile` 经 PyTorch/torchvision 导入失败。修测试加载器路径生命周期，不修改主机包、不隐藏 YOLO 用例。诊断原始 `.codex-tmp/merge-20260906/python-yolo-diagnostic-r1/`。

路径修复与真实 YOLO 联合回归 **43 passed / 3 skipped**（12.630 s），`.codex-tmp/merge-20260906/python-import-isolation-r1/`。当前全量兼容性门仍须重跑。

MVCNN 本地模型准备移入 session 临时目录：`uav_mvcnn_inputs._prepare_local_subject()` 调用既有 exporter，缓存临时目录所有者及 manifest，随后逐测试显式登记该 subject。仓库中的远端 `models/mvcnn_vehicle_cpu.manifest.json` 已恢复原字节，避免本地模型实验改写交付身份。三个受影响文件 **8 PASS**（26.610 s），原始 `.codex-tmp/merge-20260906/mvcnn-isolated-artifact-r1/`；不声明与远端注册模型二进制等价。

## Validation History

Full integration static R1 最终为 152/154 PASS、2 failed：expired-deadline failure count 与 D2h212 缺角色/oracle。原始失败完整保留，修复与 static R2 的通过证据见本文 Current Verified Checkpoint。

Full unit static R1 **759/759 PASS、63830 assertions**（53.426 s），原始 `unit-static-r1`。仍保留未配置 generated-plan smoke 不代表真实模型 qualification 的范围限制。开始 GDB 完整 integration module；不得由 unit PASS 推断整体合并完成。

Repo native build static R1 **PASS**（51.761 s）；联合 runtime closure **PASS**（0.396 s），严格断言实际 `/proc/self/maps` 中 Core、NAC、SVS 的唯一路径和两份扩展位置，并记录 SHA256。原始 `repo-build-static-r1`、`runtime-closure-static-r1`。开始最终完整 unit → integration → current Python 验证。

Core Python native build static R1 **PASS**（65.395 s），`SPEC180_NATIVE_IDENTITY_OK`；最终源码指纹与实际 Core 绑定一致。Repo 扩展随后强制重建，禁止沿用旧 User/Provider 类布局。

Build static-review R2 **PASS**（954.614 s），Core、unit/integration、DI native Provider、三个 App、readiness、assembly parity 全部完成。Real-NFD static R1 **PASS**（0.622 s）：两个不同 probe Face、同一 AA Face、HopLimit 1→0、NFD exit0；原始 `/tmp/ndnsf-controller-readiness-tdjpfc5g/`。继续重建两份扩展及核对最终加载闭包。

Controller integration focused **45/45 PASS、834 assertions**（23.106 s），包括此前12项 abort 的整个 suite；standalone readiness **13/13 PASS**（13.465 s）。原始 `controller-integration-static-r1`、`readiness-static-r1`（详细 `/tmp/ndnsf-controller-readiness-5816losz/test.log`）。DummyFace policy fixture 与完整启动门均独立通过，仍需真实 NFD 和全 module 验证。

Focused runtime checkpoint: User identity registration **3/3 PASS、9 assertions**（4.257 s）；Provider collaboration status **12/12 PASS、167 assertions**（3.282 s），包含 fetch/handler 排队撤销与四模式析构回归。原始 `identity-retry-static-r1`、`provider-lifetime-static-r1`。完整 build R2、最终绑定闭包及完整 suites 尚未完成，不能替代整体合并验收。

Certificate revocation focused **12/12 PASS、123 assertions**（0.063 s），原始 `certificate-revocation-static-r1`，包含持久化大写摘要与规范小写主体匹配、不同摘要/身份拒绝。

Cross-review / P04 / HIGH / FIXED: 新增 handler 失效分支在析构排空队列时投递未检查生命周期的失败回调。Build R1 主动中断（exit68，154.920 s），先修投递前/回调内 stopping 检查与真实排队 handler 析构回归，再复审和构建。

P04 source review resolved: 失败发布与成功 lifecycle callback 都捕获共享 stopping，并在投递前/回调内检查；新 `ProviderDestructionFencesQueuedActiveAndPostedCollaborationHandlers` 覆盖 queued、active-during-join、already-posted-success、already-posted-failure 四模式，销毁后继续 pump Face。第二审查者已完成源码与回归设计复核，未发现新确定性问题。静态复查 PASS；运行验证仍 IN_PROGRESS，开始 build-static-review R2。

Pre-build checkpoint: S02/S03/P01/P02/P03 的修复源码已落盘，`git diff --check` PASS；尚未标为验证完成。新增真实注册503重试/析构取消/8次停止三项，旧大写证书状态一项，阻塞fetch后撤销和阻塞handler后版本推进两项回归。Provider 保留 public 两参数 `fetchAndDecryptLargeData`，private `CollaborationWorkFence` 与 `fetchAndDecryptLargeDataUntil` 共享入队预算，等待/解包/安装/执行均检查权威。失败准备不安装部分已取结果。

修复完成后重新构建同源 Core/扩展，运行 focused regression、完整 unit/integration 和当前 Python 兼容性门，再运行必要 MiniNDN。Core179 撤销不等于 DI KeyGrant 撤销；Spec181 FR003 和延期边界必须在182继承中明确保留。不得据此提前勾选182原生迁移任务。
