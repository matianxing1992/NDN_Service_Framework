# Implementation Plan: NDNSF-DI Protected-Grant and Qualification Closure

**Branch**: `Experimental` | **Date**: 2026-09-05 | **Revision**: 5
**Spec**: [spec.md](spec.md) | **Status**: `IN_PROGRESS / BLOCK`

## Summary

继承 Spec180 的 ACK 规划、V3 投影、授权契约与发布工具链；目标仍是
同一候选的本地 YOLO 功能资格、exact-SIF replay 与一次 Tiger Y-B。
当前停在 G0：Python/native 授权链已接线，native Ed25519/P-256
定向正向控制和真实负例已有证据；维护的进程集成与 P-256 资源检查
已通过。T001 的请求生命周期与完整任务验收已关闭；T002 的 native
handler 源码闭包及剩余边界核对仍未完成。T007 保持 BLOCK，后续门保持关闭。

## Gate Order

```text
R0 保持未实现路径失败关闭；历史诚实化修正由所属任务持续回归
 -> G0 T001/T002/T003/T004/T006：实现、真实生产链定向验证
 -> G1 T007：当前源/配置的收敛审计 PASS
 -> G2 T005 + T008：同源 MiniNDN 七子用例与本地资格清单
 -> G3 T009 -> T010：候选封印、新 SIF、exact-SIF Y-B replay
 -> G4 T011 -> T012：一次 Tiger Y-B、唯一终局记录
```

T007 审查实现、有效配置、定向回归与后续验证设计；其 PASS 不依赖
T005/T008 的未来结果。BLOCK 期间只允许为已命名问题运行最小定向
测试，完整 unit/integration 套件、MiniNDN 全矩阵、SIF 与 Tiger 均须
先获得新 PASS。任何行为、配置、构建或 oracle 变更使下游旧证据失效。
G0 任务以 unit/定向 integration 闭合；其后续 MiniNDN 覆盖归
T005/T008，不构成 G0 的反向依赖。

T008 保留当前源身份与全部平面摘要；T009 只能封印这些相同字节，
发现源/配置漂移必须回到 T007。不得通过事后换候选把旧 PASS 晋升。

## Constitution Check

- 通用 grant、编码、密码学与生命周期归属 DI 基础设施；Core 保持通用
  服务生命周期与签名 Data 发布能力，不接管 YOLO 策略。
- 安全在真实数据路径：签名与绑定校验先于装配/加载；内容密钥必须
  消费于模型及 external-data 的加密暂存/解密；成功和失败都清理租约。
- 按设计收敛门，先核查并修复生产路径，再审计 PASS，最后正式验证。
- 遵守不可变候选晋升、冻结历史与四层证据分离；撤销、独立权威服务
  和纪元轮换仍为已接受的延期项。

## Architecture Decisions

1. **In-process authority**。权威宿主为 requester/user 进程，保持
   requester 与 authority 逻辑身份和密钥区分。签发者身份、公钥摘要
   与策略来自 `artifactPolicyAuthority`；不把发布路由身份自动视为
   策略权威。复用 `ServiceUser.publish_signed_app_data`，不新增
   Controller 权威网络服务。T001 核查 user.py 的身份覆盖与注册表一致性。
   功能切片中 `KeyGrantV1.policyAuthority` 固定为注册表 `authorityId`，
   `keyId` 使用注册表值；规范名的 authority endpoint 由 requester 的
   `deployment.user` 承载，保留 `/NDNSF-DI/KEY-GRANT/v1` 文法。
   Provider 从已封印 Selection 取得 endpoint，独立核对 payload 的
   逻辑签发者与注册表公钥。模型族/纪元先按注册表校验，具体模型允许
   列表在 grant 获取时读取可信 canonical binding 的最终 root 摘要，
   不使用发布前 package 摘要或 grant view 自述值。
2. **Authorization before assembly**。签名请求 -> 权威校验并签发 ->
   精确名发布/获取 -> Provider 校验权威、封印 grant 摘要、全部绑定与
   期限 -> recipient 解包 -> 装配/AEAD 暂存/解密加载 -> 清理。
   Merge 无 ONNX 装配身份时仍从独立已认证的 plan/grant view 取得
   模型绑定；不得把收到 grant 的自述值当作预期值。
3. **Native production path**。`NativeGrantVerifier` 和 pybind parity
   只是基础组件；T002 还须将精确名获取、受管密钥解包、授权状态、
   AEAD 消费与清理接入 `protectedRuntimeFactory` 及
   `NativeProviderHandler`。Python 诊断结果不替代 native 验收。
   factory 的注册表凭据入口必须支持与 verifier 相同的收件人类型：
   Ed25519 原始 seed 或有效 EC P-256 PEM，拒绝其他曲线/类型；
   两者保持相同公钥摘要、模型/纪元、身份映射及 0600 权限检查。
   凭据加载 unit 不替代真实 grant 发布/获取/解包验收。
   `NativeProtectedGrantCredentials.cpp` 与 `NativeProtectedGrantTransport.cpp`
   承载 factory 和维护集成测试共用的生产凭据/精确名获取入口；测试
   不复制加载或网络算法。P-256 EC 引用、point、派生 context 使用
   RAII，验证成功不代表资源释放已通过，另以 ASAN 检查。
   handler 通过请求级 `executionGuard` 把既有 `ProtectedRuntime`
   检查传入准备 worker，覆盖实际计算、缓存消费、事件/依赖发布与
   返回；取消或有效期失效在下一边界拒绝并清理，不承诺抢占模型。
   该回调仅为本地执行上下文，不扩展协议；密码学与租约仍由 runtime 所有。
   requester 与 Python Provider 使用同样的 recipient 类型约定。
   显式 `SPEC181_PROVIDER_RECIPIENT_KEY_MAP` 独立于 offer 签名密钥
   映射；仅未配置时回退到原 offer-key map，不覆盖显式配置。
4. **Protected storage**。`MODEL_PROTO` 与 `EXTERNAL_DATA` 均使用
   FR-013 既有 HKDF/AES-256-GCM 契约；暂存限定在 role workdir，
   明文全部登记租约，覆盖正常、取消和异常清理，保留 canonical 源。
   Python 消费路径以 Core 取消状态及认证 Selection 截止时间约束
   grant 获取预算，在准备、明文暴露与实际排队 worker 开始前复验，
   并同时受 grant 自身有效期约束；拒绝通过既有租约作用域清理。
   密文变异与错误密钥须到达生产解密读取边界；helper 自加密/解密
   往返不证明实际加载路径具备该拒绝能力。
5. **Independent parity claims**。T003 分别锁定 grant 解包向量与
   canonical ONNX + recipe 的装配字节/摘要向量；一者不证明另一者。
   装配比较 Python 直接入口和 C++ `prepareNativeCanonicalOnnxRole`
   完整入口；后者正常执行现有 Python 格式 helper。这里的独立性
   指 grant/装配两项验收，不声称第二套独立 ONNX 算法。固定向量
   同时检查请求序列化、external initializer、规范摘要和缓存输出。
   native helper 使用独占进程组与 runtime lease；取消/过期时先
   终止并回收进程，再清理 staging。受保护 staging 写入和最终明文
   写入持有 runtime 锁，防止取消后重新创建目录。helper 默认预算
   30000 ms，同时服从请求截止及 grant 期限；模型文件服从 role
   上界，控制 JSON、manifest 与 log 的 native 读取上界为 65536 bytes。
6. **Production-path negatives**。T006 三种变异经真实发布、获取到达
   选定 Provider，断言明确拒绝码、请求/attempt/Provider 绑定与边界。
   User 进程内直接调用 verifier 只能计作 unit probe。实验入口在正常
   requester 验证后截留一个选定 grant，再变异并更新其封印摘要与
   规范名；不替换 Provider verifier。collector 同时验证发布记录、
   `BEFORE_ASSEMBLY` verifier 记录和封印计划，资格边界标记为
   `PROVIDER_GRANT_VERIFICATION`。Y-N-E 聚合要求三个独立目录均通过。
   冷装配的 DATA_V1 等待使用调用者请求预算，仍受 native fetch 上界、
   hard deadline 与取消限制，避免额外的固定短窗口先行误报。
7. **CPU qualification**。MiniNDN 固定 canonical YOLO26n、640×640 输入
   与 CPU；只有 Tiger 要求既定 CUDA 角色及 Merge CPU 证据。

## Ownership Matrix

以下 Python 路径相对 `NDNSF-DistributedInference/ndnsf_distributed_inference/`，
native 路径相对 `NDNSF-DistributedInference/cpp/ndnsf-di/`。

| Concern | Owner and production path | Task |
|---|---|---|
| 编码、签名、信封、AEAD | `core/protected_artifacts.py`、`security/*` | T001/T006 |
| 权威宿主与发布 | `security/requester_grant_pipeline.py`、`security/registry_keys.py`、`examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`（仓库相对）；复用 Core 发布接口 | T001 |
| Python grant 消费 | `provider.py` | T001 |
| native 授权与装配 | `ProtectedRuntime.*`、`NativeGrantVerifier.*`、`NativeProviderHandler.cpp`、`NativeCanonicalOnnxAssembler.*` | T002/T003 |
| 就绪与取消 | `pythonWrapper/ndnsf/service.py`、`pythonWrapper/src/ndnsf/_ndnsf.cpp`、`ndn-service-framework/ServiceController.cpp`（仓库相对） | T004 |
| 负例、监督与身份 | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`（仓库相对）；旧 retry driver 已停用 | T005/T006 |
| 审计与晋升 | 本目录及现有 `scripts/spec180_*`、`packaging/ndnsf-di-container/jobs/spec180/*`（仓库相对） | T007--T012 |

## Shared Runtime Reuse

遵循 FR-015，直接复用 Qwen/YOLO 已有 `ModelFamilyAdapter`、
`NativeModelRunner`、runtime/worker、Selection、grant、装配及依赖
传输路径。T002 将 native 准备 factory 的证据/资源初始化提取为公共
步骤，模型分支只构造 runner spec；YOLO 后处理计算归模型 adapter。
生成 coordinator 保持既有公共角色执行基础，通过可选状态/stream
端口承载多轮差异。T007 检查多轮路径不绕过共同的授权与清理边界，
并运行受影响接口的最小定向兼容性回归；不新增 Qwen 模型资格。
当前实现与剩余工作见 [共享路径核对](evidence/shared-runtime-reuse-20260905.md)。

## Evidence and Failure Handling

每次执行使用新的 run-id，记录命令、源/构建/配置摘要、子进程退出与
首个失败边界。启动失败不能归类为授权拒绝；无证据不得把 SIGSEGV
归因为 OOM。失败先写本 Spec 记录并更新仓库 failure index，再重试。
诊断重试不得覆盖旧目录或汇总不同源上的首个 PASS 作为资格矩阵；
`retry-driver-result.json` 不是维护 runner 的正式 `y-n-matrix-result.json`。
旧 retry driver 仅保留 exit 2 的迁移提示，不执行运行或改写证据。
没有已验证的启动前可重试分类器，因此不支持自动重试。维护矩阵
在首个子用例失败后停止并保留原始结果；诊断后重跑必须使用新目录，
完整矩阵的源/构建/配置身份仍按 T007/T008 的门验证。

## Migration and Rollback

- 保持 Spec180 契约与历史证据不变；本目录记录继承范围与新失效声明。
- 每项定向修复形成独立 checkpoint；rollback 保持 protected 路径失败
  关闭，不能退回 plaintext 来通过保护用例。
- R001/R002/R004 临时门由 T006/T002/T001 对应生产验收吸收；函数存在
  或常量翻转不是删除条件。Python Provider 保护 Y-B 诊断分支由
  T002/T008 负责，native 验收后退出资格路径。
- 撤销由所有者另一分支负责，`revocationSequence=1` 只保留 wire
  兼容性；独立权威服务与纪元轮换由操作者在生产部署前集成。

## Revision History

- **6 (2026-09-05)**：按用户要求加入 FR-015，共用 Qwen/YOLO 已有
  执行机制；公共准备归一个 owner，模型算法归 adapter；T002/T007
  增加调用链与接口兼容性验收，保持 YOLO 资格范围和 12 个任务。
- **5 (2026-09-05)**：修正权威归属冲突、审计依赖环、漏掉的 Y-N-O、
  grant/装配 parity 混淆及证据等级；不扩大模型或部署范围。
