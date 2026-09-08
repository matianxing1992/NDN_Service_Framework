# T003 YOLO Fragment Identity

## Source and Review

基线 2f11a188。维护 Yolo26Splitter._candidate 的 fragment 使用规范 JSON：
candidate 为注册摘要，graph 为规划图摘要，role 为角色，nodes 为按拓扑顺序属于
该角色的节点。原生字符串哈希遗漏注册摘要与节点列表，无法绑定注册方案变化。

现复用 NativeCanonicalJson，按 graph.topologicalOrder 收集节点并计算同一 fragment，
构造器要求注册摘要有效。原生 backend 列表修正为 CPU/CUDA，安全余量改为维护
RoleResourceRequirement 默认 1.1；无 Merge 角色的原子候选不声明后处理。
旧测试显式提供注册摘要，确保 component 负例仍到达各自目标检查。

独立 oracle 实际运行维护 Yolo26Splitter.enumerate_candidates，以非字典序/Unicode
节点验证顺序及编码，并在相同 candidate ID/graph/role 下改变注册摘要，验证
fragment 身份改变。对照工件、backend、weight、margin 和 merge 字段。
注册摘要仍不是完整 SplitCandidate 摘要；此剩余身份边界保留明确注释，不把本轮
fragment 检查作为整卡 DONE。完整 semantic interface/catalog 也继续待办。

## Validation

PARTIAL。无布局变化，复用上一轮已核对新 ABI build tree；-j4 build PASS
（74.968s），82/82 cases、1084/1084 assertions PASS，包含客户端生命周期检查。
实际 Python splitter 生成两组 oracle，注册摘要改变的 fragment、非字典序/Unicode
节点及其他具名字段逐项通过。T003-B case-manifest 补齐当前五个 YOLO 用例，
避免后续卡执行只覆盖最早一个用例。design validator、git diff --check PASS。
最后仅补字段身份注释/文档，无可执行逻辑变化。未运行 integration/MiniNDN/SIF/Tiger。

Commands:
- `python3 tests/fixtures/spec182/author-yolo-fragment-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-model-descriptor-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-model-descriptor-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient,Spec182ClientState --report_level=detailed --log_level=message`

Evidence: [oracle](../../../.codex-tmp/spec182-t003-yolo-fragment-r1/oracle.log)、
[build](../../../.codex-tmp/spec182-t003-yolo-fragment-r1/build.log)、
[focused](../../../.codex-tmp/spec182-t003-yolo-fragment-r1/focused.log)、
[design](../../../.codex-tmp/spec182-t003-yolo-fragment-r1/design-validation.json)。
Context Mode active source hash 过期，使用当前仓库/CodeGraph 核对，未用旧索引判断状态。
