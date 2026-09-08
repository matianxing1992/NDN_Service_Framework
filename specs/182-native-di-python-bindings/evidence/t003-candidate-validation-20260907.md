# T003 Candidate Validation

## Source and Review

基线 42885f17。NativeSplitCandidate::validate 只检查部分角色/资源字段，未验证
graph 合法性、cut/dependency 集合和显式 rank 元数据；维护 splitter.py 的
SplitCandidate.__post_init__/validate_against 已定义这些约束。

共享 validator 现在检查 graph/model 绑定、摘要格式、配对 ingress/egress、合法
唯一 cut、已声明 dependency 角色和 dependency tensor 集合一致性；显式 degree
与 rank artifacts 完整覆盖角色，rank 工件唯一且属于本角色 artifacts。
YOLO 补齐已声明 degree=1 的 rank 工件；Qwen fixture 改用真实 hidden-layer 边名，
preparation/publisher/V3 fixture 显式提供 rank 工件，不能继续靠宽松校验通过。
默认/注入策略均由共享边界检查，不修改预算或放置准入规则。

负例逐项篡改 digest、ingress、cut、dependency、degree 和 rank artifacts，
另拒绝损坏 graph。仍未补完整 node/state/interface 和 canonical candidate identity；
本次不以局部校验冒充 hybrid plan 或完整候选等价。

## Validation

r1 build PASS（41.485s），66/68 cases PASS；两个 V3 case 的旧 SDK/native fixture
为两个 rank 复用同一 artifact，被新校验拒绝。首边界是无效候选输入，尚未执行
该 case 的 placement。修复 fixture 为独立 rank artifact，保留 SDK 独立生成预期。
复用上一轮新 ABI build tree（本轮没有布局变更），冻结同一
system compiler/Boost/NAC/SVS/ORT，单一 -j4 增量构建后运行相关原生单元。
原始目录 `.codex-tmp/spec182-t003-candidate-validation-r1/`。

r2 SDK oracle 生成 PASS；只有 rank_cover/published_rank_cover 的 core 摘要随独立
rank 工件改变。r2 -j4 build PASS（32.605s），68/68 cases、836/836 assertions PASS。
source/diff 和 design validator PASS。T003-A/B/C 保持 PARTIAL，未执行
integration/MiniNDN/SIF/Tiger，不声称 requester 或完整候选闭合。

Commands:
- `python3 tests/fixtures/spec182/author-placement-v3-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-tensor-edges-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-tensor-edges-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient --report_level=detailed --log_level=message`

Evidence:
- [r1 build](../../../.codex-tmp/spec182-t003-candidate-validation-r1/build.log)、[r1 focused failure](../../../.codex-tmp/spec182-t003-candidate-validation-r1/focused.log)。
- [r2 build](../../../.codex-tmp/spec182-t003-candidate-validation-r2/build.log)、[r2 focused](../../../.codex-tmp/spec182-t003-candidate-validation-r2/focused.log)、[oracle](../../../.codex-tmp/spec182-t003-candidate-validation-r2/oracle.log)、[design](../../../.codex-tmp/spec182-t003-candidate-validation-r2/design-validation.json)。
