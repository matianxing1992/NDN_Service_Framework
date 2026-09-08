# R1-B3 Native Merge Publication

## Design and Scope

基线 d6165c53。R1-B2 已将 Merge 类型/后处理参数绑定到候选 egress。
本批修复共享角色契约的生产者/消费者，保留规划节点集合，明确原生 Merge 不将它
解释为 ONNX 装配节点。共同 manifest/profile/graph/adapter 身份仍参与准备和封存；
ONNX initializer/assembler/ABI、装配资源上限不再是原生 Merge 的必需字段。
codec 仍检查 COMPONENT_SET、CPU、后处理参数与输入输出契约。

publisher 必须至少有一个 ONNX 角色提供经过校验的 source/assembly 资源上限；
原生 Merge 无法单独提供 ONNX canonical source publication 的信任边界。
全体角色共享 profile 与 graph，initializer digest 只与 ONNX 角色比较。
新 manifest 绑定所有角色；只重算 ONNX recipe，原生 Merge 保留候选 recipe。
该行为参照 maintained placement.py::_certify_v3_role_specs 的显式非 ONNX 分支。

使用现有 Core publication transport；不添加第二套网络、权限或装配框架。
fixture transport 属 C++ 单测，真实跨进程验证保持 T016。旧 native Merge parser
兼容范围与共同 manifest 的强制准备边界分别验证；不以仅解析成功证明生产授权。

## Validation

**NM-1/NM-2 DONE (batch only)**：r3 [build](../../../.codex-tmp/spec182-r1-b3-r3/build.log)
28.762s PASS，仅重编测试并链接；[C++ selectors](../../../.codex-tmp/spec182-r1-b3-r3/focused.log)
68 cases/800 assertions PASS，包含混合角色 inline/external 发布、V3 admission/
placement/sealing、共享 grantView manifest、protected binding projection 与 Provider
runner 配置核对，以及篡改拒绝和旧 codec/Merge 回归。
[design validator](../../../.codex-tmp/spec182-r1-b3-r3/design.json) PASS。
真实 model adapter/catalog 角色生产和 requester 仍未完成；产品卡不记 DONE。

文档 r2 检查曾遇其他会话新增 D-DESIGN-DIAGRAMS 行但 evidence 尚未生成，报告
missing link；未修改其内容。r3 在该文件存在后完整 design validator PASS。
其他会话的 Design 文件及两份 native design 修改不属于本批。

r2 [build](../../../.codex-tmp/spec182-r1-b3-r2/build.log) 25.659s PASS，仅编测试并链接；
[test](../../../.codex-tmp/spec182-r1-b3-r2/focused.log) 67/68 cases、762/763 assertions PASS。
已过 V3 core/grantView，新 fixture 的 protected epoch 与 plaintext/no-grant policy
冲突；生产 finalizeSecurity 正确拒绝。改为两角色 protected grant binding fixture，
只验证绑定与 projection，不宣称 grant 加密/传输资格。重审 policy/epoch/recipient/
role/manifest 与下游 projection 的组合，No findings；r3 继续原 selectors。

r1 incremental build 29.838s PASS，仅三份 DI 源码及对应测试文件 Compiling，Core/UAV
未重编；[build](../../../.codex-tmp/spec182-r1-b3/build.log)。
[test](../../../.codex-tmp/spec182-r1-b3/focused.log)：67/68 cases、753/754 assertions PASS。
新 case 已发布 canonical root，sealer 的 prepared role 输入为空，首边界在
NativeRequestPreparation::validateRoles；补 inputs.assemblyByRole 原始角色后重审，
原始记录不覆盖，后续 r2 验证。采样未见持续 swap-in/out。

NM-1/NM-2 STATIC_PASS / TESTS_DEFERRED；官方 review-agent 只读审查完整源码、
新增 C++ case 与离线签名作者脚本，检查 candidate egress→preparation→publication→
bindPublishedRoles→V3 sealer→grantView→projection→Provider runner metadata。
No findings. 复用既有 codec、签名 admission 和 Core publication，无新增 ABI/layout。
Python 只离线生成公开测试密钥签名 fixture（author exit=0），C++ 运行不启动 Python。
用例为合成角色契约与实际 ONNX 源字节，未声明真实 YOLO 数值或网络调用已完成。
组合审查 READY_FOR_BATCH_TESTS，尚未构建。测试覆盖 mixed inline/external、Merge 排在第一位、角色与
manifest 篡改、缺 ONNX publication anchor，及发布后 sealer/Provider 消费；
原有 ONNX-only、codec、plan/sealer、Merge runner 单测共同执行。

Context Mode active health exit=4（文档 source hash 过期），继续 repository/CodeGraph
fallback。按用户要求复用同一兼容 build tree；只重编本批三份 DI 源文件与对应测试，
最终以真实 Compiling/Linking 行核对，不按任务图总数报告全量重编。
