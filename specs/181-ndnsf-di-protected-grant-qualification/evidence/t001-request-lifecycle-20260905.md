# T001 Protected Request Lifecycle

**Status**: PASS (T001 acceptance)

## First Boundary

注册 handler 的定向回归复现 **12 failed（1.28s）**：inline/external
两种 ONNX，分别在 grant 获取前、获取过程中及准备完成后取消请求
或达到 Selection 截止时间，仍进入 handler 并加载模型。
grant 自身仍有效，因此现有 grant 过期校验无法关闭请求生命周期。
真实密码学、装配、受管租约与 ORT CPU 被调用；Core 交付、fetch 和
时钟为显式 unit fixture，不声称网络证据。原始日志保留在 ignored
workspace temporary directory 的 `spec181-python-lifecycle-20260905-r1/red.log`。

## Repair Plan

使用 Core 的取消状态和认证 Selection 的截止时间，在获取、装配、
解密暴露及实际 handler 开始前检查；fetch 预算取配置与请求剩余
预算的较小值。拒绝时仍通过既有租约清理全部受管明文。
T007 的整体审计独立于本项验收，仍为 BLOCK。

## Focused Acceptance

R2 修复后原 12 项回归与已有 handler/密码学测试共 **59 PASS（0.80s）**。
R3 新增真实 `ThreadPoolExecutor` 排队期间取消、请求截止及 grant
过期检查，以及 handler 完成模型加载后抛异常的清理；inline/external
两种 ONNX 均覆盖。handler/密码学定向集 **67 PASS（1.21s）**，
绑定、注册表、租约、收件人算法及 V3 边界回归 **80 PASS（1.18s）**。

R3 同时复验维护的实际 requester/Controller/NFD/Python Provider
进程链 **6 PASS（32.58s）**：Ed25519/P-256 正例、错误收件人、
磁盘密文变异与错误内容密钥。Selection fixture 现在明确携带请求
截止时间，消费生产 guard；本轮不声称真实网络取消注入。
所有日志保留在 `spec181-python-lifecycle-20260905-r3/`。

## Final Binding Boundary

提交前逐字段核对另发现策略快照未比较。R4 **4 failed（0.97s）**：
只改变 grant 引用或 Selection 的 `security_policy_snapshot_digest`，
inline/external 两种路径仍获取 grant 并执行模型。当时暂缓 T001
完成标记；保留 `spec181-python-lifecycle-20260905-r4/policy-red.log`。
修复为消费入口比较两份独立策略摘要，补齐原有直接调用 fixture 的
策略字段；与 native 解析入口已有比较一致。R5 全部定向集
**151 PASS（1.98s）**，再次运行真实 Python 网络消费链
**6 PASS（35.87s）**，收集 24 个进程退出（0 / 2），无遗漏。
原始 `unit.log`、`integration.log`、进程结果与退出记录保留在
`spec181-python-lifecycle-20260905-r5/`。以下完成审查据此闭合。

## T001 Completion Audit

| Requirement | Authoritative implementation and evidence | Verdict |
|---|---|---|
| Registry-pinned in-process issuance | `registry_keys.py`、`requester_grant_pipeline.py`、`grant_provider.py`；模式 0600、权威/请求方独立身份、模型/纪元与最终 root 允许列表；本轮 80 项回归及 `t001-registry-repair-20260905.md` | PASS |
| Real publication and exact fetch | 维护进程测试消费正式 `ServiceUser.publish_signed_app_data` 与 `fetch_exact_data_packet`；本轮 6 个真实网络用例；实际 user 配置链另见 `t002-p256-production-20260905.md` | PASS |
| Signature, sealed digest, all grant bindings and expiry | `_verify_protected_grant`、`verify_and_unwrap_grant` 对 provider/request/attempt/core/model/epoch、独立策略快照及权威签名、Selection 摘要逐项校验；最终 151 项回归包含继承编码基线与策略替换拒绝 | PASS |
| Authorization before assembly / model exposure | 注册的真实 handler callback 在 prepare 前调用 verifier；前置/中途拒绝不进入 handler，取消/超时未获取密钥时无装配；准备后拒绝清理已有租约 | PASS |
| Actual content-key use and disk authentication | 生产 HKDF/AES-GCM 密文暂存、读取实际磁盘、解密加载；真实网络错误密钥/密文负例在 AEAD 层拒绝；handler unit 同时覆盖 inline 与 `.weights` | PASS |
| MODEL_PROTO / EXTERNAL_DATA cleanup | 整个 callback 包裹在 `PlaintextLeaseRegistry`；成功、准备失败、handler 异常、取消、请求/grant 过期清理两类明文与内容密钥；canonical 保留；独立租约失败重试/路径替换测试通过 | PASS |
| Request and ownership bounds | fetch 使用配置与 Selection 剩余预算的较小值；准备与实际排队 worker 开始前复验时间/取消；权威模型允许列表有限、收件人文件有大小/权限检查、租约禁止重复标识且固定清理所有者 | PASS |
| Maintained source and required tests | 任务指定的生产文件与 `test_spec181_provider_grant.py`、`test_spec181_provider_grant_integration.py` 均已跟踪；本轮修改归于 T001 的本地检查点 | PASS |

T001 的完成门为 unit + 指定真实进程 integration，现已满足。
完整同源 MiniNDN 仍归 T005/T008；native handler 源码闭包归 T002，
当前总进度 4/12，T007 BLOCK。
