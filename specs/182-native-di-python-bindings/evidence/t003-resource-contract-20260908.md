# T003 Shared Resource Contract

## Changes and Review

基线 39f39a75。共享 NativeRoleResourceRequirement 原先遗漏 KV，四类预算默认零，
无法表示维护 Python 契约的未知值。修复为五类 optional uint64、默认 margin 1.1，
规范 JSON 保留完整字段；两个 splitter 显式声明 kv=0。placement/preparation
共用 peak 计算：整数求和、binary64 margin、整数截断；未知 peak 拒绝放置/准备，
超过 uint64 的预算明确拒绝。准备角色按整数向上取 MiB，避免加法溢出。

已审查共享声明、两个 splitter、所有预算消费者和聚合初始化调用点；12 组 oracle
实际调用维护 RoleResourceRequirement，覆盖五种未知字段、零、KV、截断、binary64
取整及溢出。placement/preparation 各自有 KV 容量边界与未知拒绝用例。
此为完整候选身份的前置修复，不代表完整 candidate identity 或 requester 已闭合。

## Validation

PARTIAL。fresh ABI configure PASS（6.652s），system g++ 9.4.0、ld 2.34、
Boost 1.71 系统路径及 NAC-ABE 独立 prefix 已核对；-j4 build PASS（400.678s），
84/84 cases、1119/1119 assertions PASS；12 组真实 Python resource oracle 逐项通过。
binding 源码语法编译 PASS；文档修正后 r2/r3 PASS，最终 diff 检查 PASS。
vmstat 开始及中段短采样去除首行后无持续换页，不作为全程内存峰值或加速比测量。
未启动 integration/MiniNDN/SIF/Tiger，未复用旧 Python extension 声称新 ABI 验收。

首轮文档校验拒绝 T003-C IN_PROGRESS，因为 T003-A/B 尚 PARTIAL；实际本轮修改属于
T003-A 所有的共享声明及必要消费者同步，已修正登记为 T003-A IN_PROGRESS、
T003-C PARTIAL，不改变依赖放行。首轮结果保留，非产品运行失败。

Commands:
- `python3 tests/fixtures/spec182/author-resource-budget-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-resource-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-resource-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient,Spec182ClientState --report_level=detailed --log_level=message`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/g++ -B/usr/bin -std=c++17 -fsyntax-only -I. -I/usr/local/include -I/usr/include/python3.8 -I/home/tianxing/.local/lib/python3.8/site-packages/pybind11/include pythonWrapper/src/ndnsf/di_bindings.cpp`

下一批完整候选身份需同时处理 splitter deterministic、estimated_costs、postprocessing、
hybrid_plan 及规范 execution_plan；不能将注册摘要或模型内容摘要继续充作完整候选摘要。
共享资源字段已提供完整规范序列化，可直接复用。本轮未接通客户端入口，当前
NativeInferenceClient 仍显式返回 NATIVE_REQUEST_PIPELINE_NOT_READY。

Evidence: [configure](../../../.codex-tmp/spec182-t003-resource-r1/configure.log)、
[build](../../../.codex-tmp/spec182-t003-resource-r1/build.log)、
[focused](../../../.codex-tmp/spec182-t003-resource-r1/focused.log)、
[binding syntax](../../../.codex-tmp/spec182-t003-resource-r1/binding-syntax.log)、
[oracle](../../../.codex-tmp/spec182-t003-resource-r1/oracle.log)、
[initial doc check](../../../.codex-tmp/spec182-t003-resource-r1/design-validation.json)、
[corrected doc check](../../../.codex-tmp/spec182-t003-resource-r1/design-validation-r3.json)、
[initial memory sample](../../../.codex-tmp/spec182-t003-resource-r1/vmstat-start.log)。
