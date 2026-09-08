# R2-B5 Native Group Key Admission

## Batch

基线 d13c8715；**IN_PROGRESS**。本批替换 Python `_validated_data_v1_key_offer`
及 `_native_group_epoch_key_wrapper` 的原生职责，复用 Core RSA wrap 和现有
ProviderGroupCoordinator。GB-1 同一认证 ACK 内绑定 V3 offer/key offer；GB-2 冻结
密钥/endpoint，提供 coordinator 的 Core wrap closure；GB-3 用真实 RSA KeyChain
验证 capability 投影及 Provider 解封/内层 HMAC，补错身份/epoch/摘要/namespace 负例。

新增 `NativeGroupKeyAdmission`（独立类型，无既有 ABI layout 修改）：构造接收
NativeOfferAdmission、Core ACK snapshot、NativeOfferBindingContext、nowMs；逐 ACK
重新执行已存在的 admission，禁止另一份未认证 key map。`offer(provider)` 返回
受控 admitted offer，`endpoint(provider)` 返回验证过的 namespace；`options()`
返回捕获不可变 key map 的 ProviderGroupCoordinatorOptions，wrap 只调用
Core wrapSelectionGatedInputKey，拒绝未知 Provider。它不创建 group membership 或
授权任意依赖；调用方必须从 sealed assignment 选择成员并保留 Core Selection 认证。

Write：N/NativeGroupKeyAdmission.hpp/.cpp、U/di-native-v3-placement.t.cpp、manifest、
execution-units、tasks/evidence。共用既有 V3 实际签名 fixture，给同一 ACK 加上
Core 生成的 RSA key offer，生产 admission 不允许 caller 直接创建 admitted DTO。
每成员静态门后同批一次增量 -j4，运行 Spec182V3Placement 与既有 cross-provider
group cases；不运行网络/集成。default requester 和 group operation/rank 接线由
后续 batch 完成；本批不授予完整 group dataflow PASS。

## Static Review

GB-1/2/3 STATIC_PASS / TESTS_DEFERRED；官方 review-agent 只读核对同一 ACK 的
outer evidence→V3 signature→key identity/boot/cert/public-key digest/endpoint/RSA type，
仅已认证对象进入不可变 map；closure 保留 map 寿命，Core wrap 后现有 coordinator
生成 HMAC capability，Provider 使用 Core private-key unwrap 并验证 HMAC。
复核修正 getPayload 临时范围寿命、异常断言 runtime_error、key offer wire version
及证书绝对 URI。Scope 没有引入密码实现；epochKeyId 在现有 coordinator 中是 SHA256
摘要而非密钥 hex 明文。READY_FOR_BATCH_TESTS。

## Remaining Composition Constraints

源码确认 Python group rank 是 connected component 内按 Provider 排序的编号，
不是 assembly role rank。现有 NativePlanProjectionBuilder 使用 assembly rank，
在多 stage 各自 rank=0 时不能直接作为 group producerRank。后续 group composition
须统一 endpoint、permitted operation 与 capability rank，并重新签发相关摘要。
同一 dependency 的多个 redistribution 若具有不同 kind/layout，不能共用一个
operationIndex；生成流的 stride 与 epoch 扩展也必须在同一 owner 内校验。
这些属于尚未完成的生产接线，不以当前 rank-cover fixture 推定已解决。

## Result

**DONE (local batch only)**。增量 `waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`
exit 0，28.372s，只编译 NativeGroupKeyAdmission 和 V3 placement test，再链接
unit-tests；[build.log](../../../.codex-tmp/spec182-r2-b5/build.log)。复用前批系统
g++/binutils、Boost1.71、NAC prefix 与 tokenizer target；没有 Core/UAV 编译。
vmstat 是构建结束后的采样（有效 si/so=0），不作为本批峰值内存证明。

`timeout 90s .../unit-tests --run_test=Spec182V3Placement,DistributedInferenceCrossProviderGroup --report_level=detailed --log_level=message`
exit 0，19/19 cases、1216/1216 assertions PASS；新 admission→Core RSA→Provider
capability unwrap case 有 18 条断言，见 [focused.log](../../../.codex-tmp/spec182-r2-b5/focused.log)。
测试用 Core-authentication evidence fixture 和实际 V3 Ed25519 签名/RSA certificate，
不等于运行真实 NFD Trust Schema。未知 Provider/错误 epoch-key 大小、ACK 去认证、
重复身份及 key offer 字段错配被拒绝；原有 group 13 cases 同批回归通过。

`validate_design.py` 与 diff 检查提交前通过；不运行 integration/MiniNDN。
新 admission 的本地字段拒绝抛 invalid_argument（DI_NATIVE_GROUP_KEY_OFFER_REJECTED）；
它没有引入新的 Core 网络响应或授权协议。原始 parser/crypto 异常继续抛出，不能
把它们当作已经映射到完整 requester error taxonomy；T010 负责公共错误映射。
下一步围绕上述 rank/operation 约束闭合实际 group projection owner。
