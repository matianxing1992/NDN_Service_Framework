# T004 Wire and Identity Audit

## Finding A8-01 / CRITICAL / OPEN

2026-09-07，基线 8e86bca5，在继续完整 requester 接线前发现 T004-A 的 DONE 不成立。
旧 [T004-A record](t004-a-plan-sealer-20260907.md) 的 7 个局部用例通过是真实历史，
但把自产七字段片段当 canonical oracle、把真实 parser 一致性推迟到 T016，
不能证明 T004 原要求“可被真实 Core/Provider 接受的规范计划”。T004-A 与父 T004 重开。

## Production Evidence

| Boundary | Current source | Consequence |
| --- | --- | --- |
| Selection wire | NativePlanSealer.cpp:276 encode 只输出 provider/request_id/attempt/plan_digest/plan_core_digest/ack_closed_digest/selected_role.role | NativeExecutionPlanJson.cpp:803 parser 第一门要求 schema=ndnsf-di-selection-v3、schema_version=3；现有输出必拒绝。不能仅补两个标记，后续 deadline/offer/security/roles/dataflow/device/assembly 等仍缺失 |
| Core identity | NativePlanSealer.cpp:56 canonicalCore 以自定义分隔字符串散列，未完整封印 dependencies、角色执行/device/assembly/状态语义 | 不符合 sdk/placement.py:60 canonical_bytes 与 PlacementPlanCoreV3 的既有 canonical JSON；局部重复稳定不是跨语言等价或篡改完整性 |
| Artifact binding | NativePlanSealer.cpp:180 为每个 role 计算 hash("role-artifact\|" + role) | 工件摘要来自角色名而非 ensureArtifacts/candidate 的真实认证工件；同名 role 不同工件不能据此安全封印 |
| Grant view | NativePlanSealer.cpp:206 只填 NativeProviderGrantView 前七字段 | requesterIdentity/requestId/attempt/modelManifestDigest/protectionEpoch/expiresAtMs 未绑定，不能直接交 NativeGrantClient::acquire；不能由 caller 临时补伪身份 |
| Provider projection | NativePlanSealer.cpp:258 硬编码 onnxruntime-cpu、adapter version 1；find_if 仅选择 provider 的第一个 role | 忽略实际 backend/device 与多角色/rank 覆盖；后续 T010 不能用这一入口声称规划完整 |

上表 cpp 文件均在 `NDNSF-DistributedInference/cpp/ndnsf-di/`；Python reference
在 `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py`。
所有问题均为实现/接线缺失，不能归为未知运行环境或用新 wire schema 规避。

## Bounded Native Diagnosis

为核对生产 parser 首边界，编写 `.codex-tmp/spec182-t004-wire-audit-r1/probe.cpp`，
构造当前 encode 接受的最小字段，调用真实 NativePlanSealer::encode，再调用
同库 nativeSelectionProjectionV3FromJson。未模拟 parser、未启动 Face/服务/网络。

```text
/usr/bin/g++ -B/usr/bin -std=c++17 -I. .codex-tmp/spec182-t004-wire-audit-r1/probe.cpp -Lbuild-nac182 -Wl,-rpath,/home/tianxing/NDN/ndn-service-framework/build-nac182 -lndnsf-distributed-inference -o .codex-tmp/spec182-t004-wire-audit-r1/probe
timeout --kill-after=2s 10s .codex-tmp/spec182-t004-wire-audit-r1/probe
ENCODE_ACCEPTED_BYTES=386
PRODUCTION_PARSER_REJECT=V3 Selection projection schema mismatch
```

编译 exit0、诊断 exit0 表示上述不兼容已重现，**不是产品 PASS**。raw build.log、
result.log、probe.cpp、identity.sha256 同目录保留；哈希绑定 DI 库、sealer/parser 源和诊断源。
正式 integration/MiniNDN 仍由 T016 执行；该单进程纯 codec 诊断不提前放行资格。

## Repair and Acceptance

1. 依当前 Python PlanSealerV3/PlacementPlanCoreV3/SealedPlacementPlanV3、独立冻结向量
   和生产 parser，补 native 值的完整角色/rank、dependency、device、assembly、generation、
   request/expiry/security 绑定；先修契约中的字段来源，不能发明默认值满足 parser。
2. sealCore 消费真实 candidate/artifact binding；grantView 从同一 sealed core/config
   派生完整 grant 输入；核心/最终摘要复现既有 canonical bytes，禁止自定义分隔摘要。
3. 修复 project/encode 的完整 wire；用独立 reference 向量与生产 parser 的纯单元对照
   取代“七字段片段等于自产字面量”，增加工件/依赖/device/角色篡改和多角色/rank 覆盖。
4. 修复后重审受影响 T003/T005/T008 的接线输入与局部验收。已有效局部测试不自动失效，
   但不得据其 DONE 绕过这个前置缺口；T010 依赖本修复，T015/T016 保留全链路验收。

首次文档检查 exit1：`progress dependencies not DONE: T005-A`。这确认 T005 完成状态
依赖被重开的 T004；因此 T005-A/B 随依赖回退 PARTIAL、父 T005 取消勾选，等待真实
grantView/core identity 修复后的接线复核。旧授权/发布局部 PASS 不改写，不声称这些单测失败。
当前 T004-A、T005-A/B PARTIAL；执行表 18/36 DONE、父任务 3/17。
这不是全部已勾选任务重新验收通过的声明。

依赖状态同步后 design validator exit0（18 DONE、7 NOT_STARTED、11 PARTIAL），
git diff --check PASS。plan/traceability/audit/failure index 同步。Context Mode authority
索引 helper 已写入文件源，但末尾 health 为 rc5 NO_REAL_SESSION_EVENTS；不能据此
宣称运行中 host 捕获通过，继续 repository fallback。raw context-index*.log 保留。
