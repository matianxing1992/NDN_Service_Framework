# R1-B5 Canonical Role Preparation

## Design

基线 2a041e15。现有 prepareRoles 只调用注入 port，没有生产 recipe owner；
validateRoles 错将 ONNX index 与语义 planning graph 节点数比较。本批增加
NativeCanonicalRolePreparer，输入 inspected model、实际源字节、冻结 recipe profile
和可选显式 semantic-to-ONNX 映射，不按 layer ID 猜 ONNX 节点。

RP-1：复用现有 ONNX inspector，增加 source graph 入口，产出自洽的实际 ONNX
图；原 inspectNativeOnnxPlanningGraph 继续检查预期 planning digest，不降低旧门。
RP-2：role owner 先校验 raw source/initializer size/hash、canonical identity 和
profile，然后校验每个规划节点的显式映射完整、非空、无重复且完整覆盖源节点。
只有实际 planning graph digest 相同才使用一对一映射；语义图不同必须提供映射。
边界 tensor contracts 从实际源元数据生成，ONNX recipe 使用真实 source indices。
native Merge 保留规划节点 inventory 和候选 recipe，复用 R1-B3 非 ONNX 分支。
RP-3：由真实字节产生角色、经 preparation 消费的 C++ 回归，同时保留上轮结果。

profile 包含 artifactProfileDigest、assemblerDescriptorDigest、backendAbi、precision/
quantization/layout/padding、protectionEpoch 与 source/assembled/node resource bounds。
只读 owner 捕获已检查的元数据，不保留模型字节副本；网络认证与源对象取得仍归
现有 Core/catalog owner。原始 model 参数不会由 profile 覆盖或合成来源名。

普通 ONNX role 的 prepare 阶段索引须小于 maxNodes（源节点资源上限）；实际 source
节点数和映射在生产 owner 检查，装配器仍对实际 ONNX 节点数检查。native Merge
inventory 继续按 planning graph 检查。不再以语义节点数量冒充 ONNX 节点数量。
既有 class layout 不变，复用兼容 build tree，新增 API/owner 不触发 Core/UAV 重编。

## Validation

RP-1/RP-2/RP-3 逐项官方静态门后共享 -j4 build，选择
Spec182Preparation/Spec182NativePlanning/Spec182CanonicalPublisher/Spec182V3Placement
及既有 ONNX recipe/extraction unit selectors。不运行 integration/MiniNDN。

RP-1/RP-2/RP-3 STATIC_PASS / TESTS_DEFERRED；已加载官方 review-agent，只读核对
完整新 API/owner/test 差异、原 source inspector 入口、Python _v3_role_kind/
_role_layer_range、source→role→preparation→publisher/recertification→实际 extractor。
审查发现 layer-0/layer-00 可被 set 折叠成一个数值层，已显式拒绝并补负例；重审
No findings. JSON 库复用现有实现，未改变既有 class layout 或认证所有权。
RP-3 使用冻结 inline/external 实际 ONNX bytes，单个语义节点映射两个实际节点，
要求 role producer 输出可抽取且 artifact digest 与既有独立 ONNX fixture 相同；
同时覆盖无映射、越界、重复/遗漏、外来源/manifest/state、取消和相同坐标自动映射。
组合审查 READY_FOR_BATCH_TESTS。此为源检查与 recipe producer，不是实际 catalog
网络认证、Qwen state 名称适配、tensor-rank 源分片或默认 requester 全链验收。

## Result

**DONE (batch only)**。实际执行 `python3 ./waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`，
沿用该树的 system compiler/binutils、Boost 与 tokenizer bridge 配置，exit 0，46.998s。
[build.log](../../../.codex-tmp/spec182-r1-b5/build.log) 记录 11 条 Compiling 和 unit-tests 链接；
仅 DI/ONNX 与受头文件影响的测试对象，Core/UAV 未重编。

该树 unit-tests 执行 `--run_test=Spec182Preparation,Spec182NativePlanning,Spec182CanonicalPublisher,Spec182V3Placement,Spec182OnnxExtraction --report_level=detailed --log_level=message`，
exit 0，**69/69 cases、1560/1560 assertions PASS**，见
[focused.log](../../../.codex-tmp/spec182-r1-b5/focused.log)。
inline/external 两种源均由真实 producer 生成 recipe，经 publication/rebinding 后
再次实际抽取，制品摘要与独立冻结 ONNX fixture 一致；负例覆盖 RP-3 所列边界。
源码时间戳均早于本次测试日志；收尾未修改源码或重复构建。

`python3 specs/182-native-di-python-bindings/checklists/validate_design.py` PASS，见
[design.json](../../../.codex-tmp/spec182-r1-b5/design.json)；提交前另检查最终文档与 diff。
Context Mode project health PASS，active health exit 4（spec/plan/tasks 索引摘要过期），
使用仓库与上述原始日志作为 checkpoint 权威，不据过期检索判断进度。
T003/T008 原卡及完整 Spec 保持原验收状态；本批不证明真实 catalog 获取、Qwen
state/rank source 适配或默认 requester 接线。下一步按原卡核对剩余生产端口依赖。
