# T003 Complete Candidate Identity

## Changes and Review

源码基线 919dc689。原生 candidateDigest 原先仅绑定少数字段或直接复用 YOLO 注册摘要，
无法绑定完整切分候选。现补 splitter deterministic、estimatedCosts、postprocessingJson、
NativeHybridPlan；完整规范序列化覆盖维护 SplitCandidate 全部字段，validate 强制重算比对。
资源字段复用上一批 canonicalJson；图状态张量保留形状、未知字节与有序数组。

HybridPlan 复用运行时 RedistributionSpec，校验 canonical rank labels、完整 stage cover、
相邻边界、操作/rank 形状、布局/完整性摘要、重复/未知 rank、完整输出和 degree-change
重分片义务。此为候选契约；不代表默认 Qwen splitter 的多 rank 执行已完成。

两个默认 splitter 计算完整候选摘要；YOLO 注册摘要仍用于 fragment，普通候选保留
空 rank maps，preparation 解释为 rank-one。成本估算保留维护语义；只有显式 Merge
接收 adapter 提供的 postprocessing JSON，严格拒绝非 object/重复键。

候选 execution_plan 规范域是 roles/dependencies/node_roles；现有 NativeExecutionPlan
中的请求 transport/control 字段由后续 sealed plan 绑定，不混入模型候选身份。
原生 group dependency 按 producer/consumer 顺序展开为维护 RoleDependency 列表。
审查覆盖声明/序列化/验证、两个 splitter、preparation 和 V3 seal 输入；原有负例保留。
资源容量/角色顺序的合法测试变更显式刷新摘要，避免在旧摘要处提前失败而掩盖真正边界。

## Validation

PARTIAL。真实 Python 生成 9 组完整候选（含 Qwen splitter、4 类 hybrid）、2 组
实际 YOLO splitter 全候选字节；V3 oracle 用实际 SplitCandidate 摘要重新生成 core。
原生检查逐字节/摘要比对，20 类合法字段变更须旧摘要拒绝、刷新后通过；14 类 hybrid
负例及普通 rank-one preparation 正例已编写。fresh ABI configure PASS（7.427s），
system compiler/binutils、系统 Boost 与 NAC-ABE prefix 已核对，-j4 build PASS（406.080s）。
r1 focused rc=201：86/87 cases、1244/1246 assertions PASS。差异递归比对定位为原生
Qwen 额外填写 inputIngressRole/resultEgressRole，实际 Python Qwen splitter 保留空值。
按维护算法移除这两个赋值；原测试改查空值，unpaired ingress 负例显式构造不成对值。
不修改 Python oracle 的期望，不绕过后续 sealed plan 的 owner/ingress 验证。
r2 增量 build PASS（26.152s），87/87 cases、1246/1246 assertions PASS；binding
源码语法编译 PASS，design validator 与 git diff --check PASS。首轮结果原样保留。

Commands:

- `python3 tests/fixtures/spec182/author-candidate-oracle.py`
- `python3 tests/fixtures/spec182/author-yolo-fragment-oracle.py`
- `python3 tests/fixtures/spec182/author-placement-v3-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-candidate-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-candidate-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient,Spec182ClientState --report_level=detailed --log_level=message`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/g++ -B/usr/bin -std=c++17 -fsyntax-only -I. -I/usr/local/include -I/usr/include/python3.8 -I/home/tianxing/.local/lib/python3.8/site-packages/pybind11/include pythonWrapper/src/ndnsf/di_bindings.cpp`

没有运行 integration/MiniNDN/SIF/Tiger；完整候选身份完成后仍需 graph adapter 绑定、
catalog/interface、实际源图 inspection/装配映射和 requester 接线，任务保持 PARTIAL。
Context Mode active tasks 索引过期，使用当前仓库与 CodeGraph，不依赖旧 checkpoint。

Evidence: [candidate oracle](../../../.codex-tmp/spec182-t003-candidate-r1/candidate-oracle-final.log)、
[YOLO oracle](../../../.codex-tmp/spec182-t003-candidate-r1/yolo-oracle.log)、
[V3 oracle](../../../.codex-tmp/spec182-t003-candidate-r1/placement-oracle.log)、
[configure](../../../.codex-tmp/spec182-t003-candidate-r1/configure.log)、
[build](../../../.codex-tmp/spec182-t003-candidate-r1/build.log)、
[first focused failure](../../../.codex-tmp/spec182-t003-candidate-r1/focused.log)、
[r2 build](../../../.codex-tmp/spec182-t003-candidate-r2/build.log)、
[r2 focused](../../../.codex-tmp/spec182-t003-candidate-r2/focused.log)、
[binding syntax](../../../.codex-tmp/spec182-t003-candidate-r1/binding-syntax.log)、
[design check](../../../.codex-tmp/spec182-t003-candidate-r1/design-validation.json)。
