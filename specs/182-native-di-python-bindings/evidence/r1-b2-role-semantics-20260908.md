# R1-B2 Candidate Role Semantics

## Boundary

基线 59993844。候选摘要已经包含 mergeKind、resultEgressRole 和 postprocessingJson，
但 NativeRequestPreparation::validateRoles 没有将这些内容与实际角色绑定。
本批复用该入口，防止 prepareRoles、ensureArtifacts 和 canonical publisher 消费
被替换的角色语义。独立判据为 maintained placement.py::_role_specs_for_candidate
对 egress 的字段投影，以及 C++ 单字段篡改拒绝。

## Remaining Chain

源码核对发现另一个尚未修复的问题：Python _certify_v3_role_specs 对原生 Merge
跳过 ONNX certification；C++ validateRoles、publisher、bindPublishedRoles 和
selectionRoleFromV3Json 仍要求 ONNX identity/recipe，PlanSealer 复用同一 validator。
后续须统一原生 Merge 身份、共享 manifest 绑定与发布后重认证，保留 Provider
native-yolo-postprocess 的输入输出及授权约束，不能仅跳过验证。本批不声明该路径可用。

## Validation

最终 r2：单次增量 -j4 [build](../../../.codex-tmp/spec182-r1-b2-r2/build.log)
16.033s PASS；[C++ focused](../../../.codex-tmp/spec182-r1-b2-r2/focused.log)
26/26 cases、546/546 assertions PASS。CR-1/CR-2 DONE，仅关闭本批角色语义绑定；
T003-C 等原卡状态不变。构建采样 vmstat 后两行 si/so 均为 0，无持续换页。

首次 build 16.928s PASS；25/26 cases、532/534 assertions PASS。新 fixture
漏填 inputIngressRole，被 candidate.validate 首先拒绝，尚未到新增校验。
原始 [build](../../../.codex-tmp/spec182-r1-b2/build.log) 与
[test](../../../.codex-tmp/spec182-r1-b2/focused.log) 保留。
补齐成对 ingress/egress，重审 fixture 生命周期及全部 mutations，无其他发现；
失败只属于测试数据，产品批次保持 PARTIAL，下一轮使用独立 r2 日志。

CR-1 STATIC_PASS / TESTS_DEFERRED；当前执行者已加载官方 review-agent，以只读模式
审查完整源码/test diff、validateRoles 的四个调用点与 maintained egress 投影。
No findings. 组合入口复用同一校验，没有 ABI/layout 或异步所有权变化。
CR-2 READY_FOR_BATCH_TESTS；C++ case 覆盖五字段分别在 egress/non-egress 篡改、
移动 egress，以及 ONNX merge/普通默认投影。现有 manifest 的 Preparation/* selector
包含新 case。工具链 cache：g++ 9.4.0 -B/usr/bin、ld 2.34、system Boost 路径已核对，
使用兼容的 spec182-yolo-semantic-r1/build；无布局变化，无需 fresh ABI rebuild。

Context Mode strict active health exit=4，索引 hash
过期；使用 feature pointer、当前文档及 CodeGraph canonical source fallback。
