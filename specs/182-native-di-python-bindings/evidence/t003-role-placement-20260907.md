# T003 Role Placement Repair

## Current Status

2026-09-07 / PARTIAL。逐角色独立 Provider 分配、目标工件摘要提示排序、proposal 可行性
复核及预算溢出拒绝已实现。完整 device/rank/residency proof DTO 与 frozen Python 对照仍待完成。

## Attempt r1

原始 `.codex-tmp/spec182-t003-placement-r1/`：build.log exit 0（13.02s）；focused.log exit 201。
首边界有两种，均在 fixture：sealer helper 依赖无关 resident 摘要赢过更大 freeBytes，
新规则正确选择 provider-b，11 个测试因此在 helper 前置断言停止；新增双角色 Qwen fixture
误传一个 tensor degree，4 个测试在 splitter 构造时被精确覆盖检查拒绝。
修复 fixture 为目标 artifact digest、每角色一个 degree；不恢复无关 residency 优先级。
本轮失败不是网络/协议结果。重试使用新 r2 目录，不覆盖 r1。

## Attempt r2 / Focused PASS

`.codex-tmp/spec182-t003-placement-r2/build.log`：同一已验证 ABI build 目录，
`python3 ./waf -o .codex-tmp/spec182-t004-bindings-r1/build build --targets=unit-tests -j4 -v`，
exit 0，14.68s；本轮未改变公开 DTO 布局。focused.log：新 unit-tests 使用
`--run_test=Spec182NativePlanning,Spec182PlanSealer,Spec182Preparation --report_level=detailed --log_level=message`，
exit 0，37/37 cases、290/290 assertions。design-validation.json 无 errors，git diff --check PASS。
没有启动全量回归、集成或集群实验；该结果只关闭本次分配修复，不关闭 T003-C 整卡。

## Source Review and Remaining Contract Work

reference planner/presplit_first.py::propose_v3 按角色顺序贪心、跳过 used_providers；
当前修复沿用该结构，不引入回退搜索。canPlaceRole 先能力过滤再预算检查，排序只影响可行集合。
摘要提示必须覆盖本角色全部工件，仅作为 canonical availability 提示，不能证明 exact loaded reuse。
NativePlacementProposal::validate 同时拒绝重复、外来或资源不满足的 Provider。
更新实际 suite selector Spec182NativePlanning；旧 Spec182Placement 并不存在。

Context Mode 项目/active health 均为 NO_REAL_SESSION_EVENTS（exit 5），采用仓库权威文档
与 CodeGraph 实际源码作为依据；未尝试 purge 或伪造 host 验收。
