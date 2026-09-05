# Implementation Plan: NDNSF-DI Protected-Grant and Qualification Closure

**Branch**: `Experimental` | **Date**: 2026-09-05 | **Revision**: 5
**Spec**: [spec.md](spec.md) | **Status**: `IN_PROGRESS / BLOCK`

## Summary

继承 Spec180 的 ACK 规划、V3 投影、授权契约与发布工具链；目标仍是
同一候选的本地 YOLO 功能资格、exact-SIF replay 与一次 Tiger Y-B。
当前停在 G0：Python 授权链只有部分接线与 unit 证据，native verifier
尚未接入 native Provider 运行时；后续门保持关闭。

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
2. **Authorization before assembly**。签名请求 -> 权威校验并签发 ->
   精确名发布/获取 -> Provider 校验权威、封印 grant 摘要、全部绑定与
   期限 -> recipient 解包 -> 装配/AEAD 暂存/解密加载 -> 清理。
   Merge 无 ONNX 装配身份时仍从独立已认证的 plan/grant view 取得
   模型绑定；不得把收到 grant 的自述值当作预期值。
3. **Native production path**。`NativeGrantVerifier` 和 pybind parity
   只是基础组件；T002 还须将精确名获取、受管密钥解包、授权状态、
   AEAD 消费与清理接入 `protectedRuntimeFactory` 及
   `NativeProviderHandler`。Python 诊断结果不替代 native 验收。
4. **Protected storage**。`MODEL_PROTO` 与 `EXTERNAL_DATA` 均使用
   FR-013 既有 HKDF/AES-256-GCM 契约；暂存限定在 role workdir，
   明文全部登记租约，覆盖正常、取消和异常清理，保留 canonical 源。
   密文变异与错误密钥须到达生产解密读取边界；helper 自加密/解密
   往返不证明实际加载路径具备该拒绝能力。
5. **Independent parity claims**。T003 分别锁定 grant 解包向量与
   canonical ONNX + recipe 的装配字节/摘要向量；一者不证明另一者。
6. **Production-path negatives**。T006 三种变异经真实发布、获取到达
   选定 Provider，断言明确拒绝码、请求/attempt/Provider 绑定与边界。
   User 进程内直接调用 verifier 只能计作 unit probe。
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
| 就绪与取消 | `pythonWrapper/ndnsf/service.py`、`ndn-service-framework/ServiceController.cpp`（仓库相对） | T004 |
| 负例、监督与身份 | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、`scripts/run_spec181_y_n_matrix_retry.py`（仓库相对） | T005/T006 |
| 审计与晋升 | 本目录及现有 `scripts/spec180_*`、`packaging/ndnsf-di-container/jobs/spec180/*`（仓库相对） | T007--T012 |

## Evidence and Failure Handling

每次执行使用新的 run-id，记录命令、源/构建/配置摘要、子进程退出与
首个失败边界。启动失败不能归类为授权拒绝；无证据不得把 SIGSEGV
归因为 OOM。失败先写本 Spec 记录并更新仓库 failure index，再重试。
诊断重试不得覆盖旧目录或汇总不同源上的首个 PASS 作为资格矩阵；
`retry-driver-result.json` 不是维护 runner 的正式 `y-n-matrix-result.json`。

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

- **5 (2026-09-05)**：修正权威归属冲突、审计依赖环、漏掉的 Y-N-O、
  grant/装配 parity 混淆及证据等级；不扩大模型或部署范围。
