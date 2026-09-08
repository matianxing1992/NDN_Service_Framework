# R2-B6 Authorized Group Projection

## Design and Batch

基线 9e5849bf，IN_PROGRESS。GP-1 统一 endpoint/dataflow 重新认证 helper；
GP-2 新 NativeGroupProjectionBuilder 从 sealed dependency 的 connected components
构造成员、操作和 capability，复用 NativeGroupKeyAdmission/Core RSA；GP-3 C++
group/model rank 分离、多个 tensor/redistribution、Provider capability/manifest 消费正负例。
多 stage 各自 rank=0 是该分离规则的动机；本批具体向量通过交换两个 rank 的 Provider
分配构成 rank 不同的真实场景，不声称已运行真实多 stage 模型。
同批静态门后一次兼容树增量 -j4，运行 V3Placement/PlanSealer/cross-provider-group。

`build(sealed,candidate,keys,context,maxBytes)` 返回完整 role projections，context
只接受请求时间/输入身份/segment budget，不允许 caller 预填 dependency/capability。
从 selected roles 确定 component，Provider 排序确定 group rank，group id 绑定
plan/component/member list；每个 dependency transfer（包含 tensor/kind/layout identity）
分配唯一 operation index，同一 transfer 的多个 producer 共用 operation。endpoint
改用 group rank；所有 manifest/endpoint/readiness/dataflow digest 统一重算。
capability 的 permitted operations 与实际 endpoint 同源；Core 负责 wrap，现有
coordinator 负责 HMAC/Provider projection。局部 coordinator 返回前析构清除 epoch key。

返回值不改变封存计划/candidate，也不重新授权模型密钥。generation stride 若启用，
须覆盖基础操作集合并做有界 epoch 扩展。TOKEN_FEEDBACK 的专属 endpoint/流式状态
还由 R4 generation owner 接入；本入口遇到它明确拒绝，不能静默漏授权后宣称支持
完整 generation。该边界不是删除 T010-C/T011/T016 的要求。

Write：N/NativeGroupProjectionBuilder.hpp/.cpp、NativePlanProjectionBuilder.hpp/.cpp
共享认证 helpers、V3 placement test、manifest、execution-units、tasks/evidence。
原始无 capability 的 projection builder 仍供静态准备/离线对照；默认 requester
跨 Provider 生产路径必须使用新 owner，不能用旧测试 context 冒充真实 group 授权。

## Static Review

GP-1/2/3 STATIC_PASS / TESTS_DEFERRED；官方 review-agent 只读核对完整差异：
shared certify 保留既有 manifest domain 和 application-input identity，ROLE remap 后
统一重算 endpoint/ALL wait/dataflow；union component/rank 与 exact selected provider
覆盖一致。transfer key 含原 dependency index、tensor、kind、integrity/layout，避免
不同 transfer 共用 operation；同 transfer 的多 producer/consumer 合并 rank 集。
生成 epoch count/stride 做溢出及 Selection 1 MiB wire 预检，capability 最终编码再验限。
临时 coordinator 自身析构清除 secret，返回只含各 Provider 的单一 envelope 投影。
C++ case 交换两个 Provider 分配，使 model rank 与 group rank 确实不同，随后经
Core RSA 解封两端 capability，实际 sealOperation/openSegment 回到原始字节；另含
两个 redistribution 不同 layout/operation index、caller 预填授权/过期/feedback 拒绝。
READY_FOR_BATCH_TESTS；尚未执行 generation 或默认 requester/network 验收。

## First Build Boundary

r1 build exit 1：fixture 的 proposal 是 const，新增 swap/restore Provider 分配
无法编译，未执行测试；[build.log](../../../.codex-tmp/spec182-r2-b6/build.log)。
只调整该测试局部变量可变性，静态复核交换前后恢复和 seal 输入绑定；用新 r2
目录保留后续日志，不清理/重建 Core/UAV。

## Final Result

**DONE (initial-request local batch only)**。r2 build exit 0，24.302s，仅重编测试文件
及链接；首轮已编译 NativePlanProjectionBuilder/NativeGroupProjectionBuilder，因
test const 错误未成功完成，不能省略该尝试。两次原日志分别见
[r1 build](../../../.codex-tmp/spec182-r2-b6/build.log) 和
[r2 build](../../../.codex-tmp/spec182-r2-b6-r2/build.log)。继续复用 system g++/binutils、
Boost/NAC/tokenizer closure 和兼容树，Core/UAV 无重编。

`timeout 90s .../unit-tests --run_test=Spec182V3Placement,Spec182PlanSealer,DistributedInferenceCrossProviderGroup --report_level=detailed --log_level=message`
exit 0，31/31 cases、1305/1305 assertions PASS，见
[focused.log](../../../.codex-tmp/spec182-r2-b6-r2/focused.log)。本次新增组合位于
`ProjectionBuilderDerivesApplicationInputAndDependencyReadiness`，覆盖排序成员 rank、
两个 tensor operation、Core RSA capability 安装、实际 sealOperation/openSegment、
两个不同 layout 的 redistribution、caller 授权污染/expiry/feedback 拒绝。
`check-projection-wire.py` 对原有 7 个 dataflow/11 个 endpoint 的 SDK 独立校验仍 PASS，
见 [sdk-check.json](../../../.codex-tmp/spec182-r2-b6-r2/sdk-check.json)；此 SDK 结果
证明共享 certify 没有破坏既有规范对象，不冒充新 group graph 的独立完整 oracle。

初次 vmstat 有一次 si=8 KiB/s、下一次为 0，无持续 swap；不宣称峰值内存验收。
最终 design validator/diff 检查通过。未运行 integration、MiniNDN、真实 generation。
T004/T010 仍未整体验收；下一步进入 R3 默认 requester 调度，把已实现的 catalog、
preparation、placement、grant 与 group projection 连接到真实 Core commit/response。
R4 必须接上 TOKEN_FEEDBACK/streaming operation 消费，不能用本批限制删除该功能。
