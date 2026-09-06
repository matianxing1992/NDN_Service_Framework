# Static Review Gate Revision 4

**Date**: 2026-09-06 | **Status**: DOCUMENT_REVIEW
**Baseline**: `b5eb4d396bbfa711c5b246669e0e74c89c23a2a6`
**Scope**: 设计技能/审计/实施入口与Spec182；production STATIC_REVIEW NOT_RUN，runtime NOT_RUN。

## Intent and Changes

用户要求在unit/integration/MiniNDN之前先阅读代码逻辑、对照设计、发现并修复静态可见的问题。
旧T015只位于正式资格前，不能覆盖单元最早的focused运行；原L0又把Static/compile合写。
本次新增S0独立语义审查，嵌入全部17任务，保留T015整体范围及T016依序unit→integration→MiniNDN。

写作技能增加pre-test-static-review参考，模板/工作单元/审查清单同步；specify/plan/tasks入口强制设计此门，
audit增加pre-test-static模式与报告，implement在实际执行前要求fresh scoped PASS。
报告包含源码/设计/测试subject、逐条逻辑推理、finding、修复复审、允许范围及失效。
具名RED/mutant也先审查受控缺陷，只允许对应检错case，不放行正常测试或宣称产品符合设计。

技能属于本地配置，沿用仓库规则不纳入源库checkpoint。本次可提交成果为Spec182规范、文档验证器及证据。
没有修改运行实现、操作合并工作区索引、运行unit/integration或模型实验；也没有给planned实现签发代码审查PASS。

## Source and Retrieval Boundary

当前工作区Spec182在开始时干净，索引为空。读取活动指针、spec/plan/tasks/contracts、设计收敛指南、
架构阅读指南及failure index。最新相关raw显示交付工具40PASS与counterfactual预期DID NOT RAISE失败；
这仅用于理解已有边界，本轮未重跑或修改其结果。合并修复进展不由本次文档改动更新。
已有revision3合并source快照保留历史身份。

Context Mode stats只作异常筛查；project health PASS，active health初次exit5
（NO_REAL_SESSION_EVENTS），因此直接读取持久仓库文档，不用自动记忆作checkpoint authority。
本次没有改变生产符号/接线，也不宣称完成产品CodeGraph调用路径审查。

## Review of This Change

逐项对照用户意图和修订diff：
- S0在各单元首次运行前，T015不再被误用为唯一静态阶段。
- 静态审查明确读源码/受影响caller/测试/collector，而非用lint、构建或格式扫描冒充。
- BLOCK修复复审，source/test/config等变化使相关结论STALE，范围不足不授权更广测试。
- 保留全部既有运行PO，静态PASS不消除unit/integration/MiniNDN义务。
- 保持17任务依赖，FR-018/SC-010/PO-015/SR-001--009及每任务StaticReview可追踪。
- 纯设计阶段的产出是门禁规范；没有实际产品审查或运行证据。

新文档验证器在执行前已检查逻辑：读取静态文件、核对ID/依赖/链接/条目和报告字段，不执行产品代码，
不把文字覆盖计为语义代码审查。其合法输出明确product_static_review=NOT_RUN。
审查覆盖输入路径、计数、任务解析/依赖环、报告字段和退出条件；文档未缺项时应exit0，
缺必需StaticReview/报告字段时应exit1。检查器自身的运行是文档结构检查，不是产品unit test。

## Validation and Next Action

实际检查结果见 [revision 4 validation](revision4-validation.json)。
文档契约验证PASS（18 FR / 10 SC / 15 PO / 17 tasks / 186链接）；严格结构审计及diff检查PASS，技能frontmatter/引用/入口检查PASS。两个只在内存中删除字段的counterfactual分别以exit1指出T002缺StaticReview、报告缺AllowedTestScope；未写临时变异文件，未运行产品测试。活动上下文重新索引后的健康结果另列于上述JSON，不用该结果替代源码审查。
实施0/17；STATIC_REVIEW产品NOT_RUN。下一步合并基线稳定并关闭T001设计门，
各单元按实现→S0修复复审PASS→unit→integration推进，最后T015整体审查→T016正式验收。
