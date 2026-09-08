# R2-B3 Production Projection Builder

## Design and Members

基线 61b73281。PB-1 复用 NativeSelectionJson 的 endpoint/dataflow JSON 值生成，
避免第二套 wire serializer。PB-2 新增 NativePlanProjectionBuilder，从 sealed plan、
原完整候选和 admitted offers 生成每角色 execution/dataflow/device inputs；角色分配、
拓扑、resource snapshot/sequence 不再由测试 fixture 或 Python backfill。

request owner 显式提供固定 nowMs、no-progress/segment 上限、可选 application-input
identity，以及 Core group owner 的 dependency transport metadata/capability。这些是
已有责任边界的输入，不由 builder 签发或伪造授权。支持普通多 tensor dependency、
redistribution 与多消费者共享 endpoint identity；TOKEN_FEEDBACK 保留在封存执行计划，
不加入单 epoch readiness DAG。terminal 从候选声明或唯一 sink 得出，未知/不唯一拒绝。

PB-3 C++ 实际 V3 placement/sealing 消费验证及独立 SDK wire 对照。输出后仍由原
NativePlanSealer::project/encode 和 Provider 校验；本批不替代真实 grant、GroupCapability
或默认 requester。新增类/方法，不修改现有 class layout；批末共享 V3Placement、
PlanSealer、CanonicalPublisher、NativePlanning、Preparation，兼容树增量 -j4。
IN_PROGRESS；T004/T008/T010 仍保持未完成。

## Static Review

PB-1/PB-2/PB-3 官方 review-agent 只读审查覆盖共享 wire 值函数、新 builder、原
sealer/grantView/admission、SDK TensorEndpoint/RoleDataflow/DeviceBinding 与测试。
发现 redistribution 不能向 dependency 内所有 rank 广播；已按每条 transfer 的
producer/consumer rank cover 过滤，并拒绝未知/重复/歧义 rank，补正负例。
组 namespace 额外 provider cover 拒绝；endpoint identity 删除本地 consumer_role，
保留 consumer_roles，使多消费者共享对象；dataflow digest 删除自身派生字段。
No remaining findings，STATIC_PASS / TESTS_DEFERRED，READY_FOR_BATCH_TESTS。

独立 `author-projection-oracle.py` 直接构造维护 SDK DeviceBinding/RoleDataflowContract，
使用现有签名 offer/placement oracle，生成 4 组完整规范字段/摘要，exit 0。C++ 测试
通过真实 admission→placement→sealCore 后消费 builder；graph 用例覆盖 application
input、双 tensor pipeline、ALL readiness、唯一 terminal、缺组/cycle/feedback、rank
redistribution。grant 仅元数据 fixture，不冒充真实 grant 签发或 Provider 密钥验收。

首轮 build 42.473s PASS，73 cases/1735 assertions PASS。收尾发现非空 endpoint/dataflow
摘要尚无独立 SDK decoder 校验；增加 test-only 可选 raw exporter 和 offline checker。
不改产品行为，静态核对文件仅由显式环境变量指定，不读取产品秘密；增量只重编
该测试，重跑一个 graph case 生成新 raw，再由维护 RoleDataflowContract.from_bytes
校验嵌套 endpoint/整体 dataflow 摘要与规范往返，不把空 dataflow 对照扩大为全证明。

## Final Result

**DONE (batch only)**。首轮共享 `waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`
exit 0，42.473s，8 个受影响 DI/测试对象与 unit-tests 链接；
[build.log](../../../.codex-tmp/spec182-r2-b3/build.log)。沿用 system compiler/binutils、
system Boost 与既有 tokenizer target，Core/UAV 未重编；未观察到持续换页。
同树执行 `timeout 90s .../unit-tests --run_test=Spec182V3Placement,Spec182PlanSealer,Spec182CanonicalPublisher,Spec182NativePlanning,Spec182Preparation --report_level=detailed --log_level=message`
exit 0，**73/73 cases、1735/1735 assertions PASS**，
[focused.log](../../../.codex-tmp/spec182-r2-b3/focused.log)。

补充 test exporter 后兼容增量 build exit 0、29.547s，仅测试对象与链接，见
[r2 build.log](../../../.codex-tmp/spec182-r2-b3-r2/build.log)。设置
`NDNSF_PROJECTION_ORACLE_OUTPUT=.codex-tmp/spec182-r2-b3-r2/projections.jsonl` 的绝对路径，
运行 `Spec182V3Placement/ProjectionBuilderDerivesApplicationInputAndDependencyReadiness`，
exit 0，**1 case/37 assertions PASS**，[r2 focused.log](../../../.codex-tmp/spec182-r2-b3-r2/focused.log)。
`python3 tests/fixtures/spec182/check-projection-wire.py .codex-tmp/spec182-r2-b3-r2/projections.jsonl`
exit 0，**7 dataflows/11 endpoints** 经维护 SDK 解析校验和规范往返通过，每份篡改
dataflow digest 均拒绝；[sdk-check.json](../../../.codex-tmp/spec182-r2-b3-r2/sdk-check.json)。
Python 仅承担 offline independent oracle，不承担运行逻辑或主要行为测试。

`validate_design.py` 与最终 diff 检查通过；case-manifest 已登记新实际 graph case。
Context Mode active hash 过期，采用 canonical 仓库与原日志。原 T004/T008/T010 和
最终资格不因本批关闭：group/capability context 必须接入真实 Core group owner，
grant 元数据 fixture 不能代表签发，默认 requester 尚须实际调用该 builder 再 project/
encode/commit。未运行 integration、MiniNDN/SIF/Tiger；两份并发设计改动保持隔离。
