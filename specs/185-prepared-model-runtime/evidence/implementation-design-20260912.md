# Implementation Design and Skill Enforcement

**Status**: DOCUMENT_REVIEW_PASS / IMPLEMENTATION_NOT_STARTED
**Baseline**: 6a29dbe8 / Experimental；生产源码与native编译/运行均未修改或执行。

## Existing Requirement and Actual Gap

原shared skill的symbol-contract/design-template/work-unit-contract已经要求具体类/函数/字段、前后签名、调用和失败流程；不是完全没有规则。
Spec185虽有C-01–C-07公开API及领域契约，部分实现任务只写提取/接入/实现，缺内部设计绑定。
本轮强化任务开始前Design binding，补[C-08](../contracts/code-design.md)和16任务逐项引用，不能把“以后编码时设计”当READY。
冻结关键职责/签名/owner/提交点/失败与迁移，普通无行为影响局部实现保持LOCAL_DETAIL，不加批准环节。

## Skill Changes and Installed Copies

- 版本化shared SKILL、work-unit-contract、design-template、review-gate加入Design binding检查。
- plan/tasks模板同步；本机speckit-plan/tasks/implement/audit入口明确引用同一契约。
- 个人`/home/tianxing/.codex/skills/speckit-code-design/`的上述4文件已同步；权威源码仍为repo skills目录，个人安装不是额外产品进度。
- 首次checkpoint被仓库commit hook拒绝：`.agents/skills/`本机入口不允许纳入Git。已仅撤下这4项暂存，保留其本地安装修改；版本化shared skill、模板及同步检查器正常交付，不绕过hook。其他机器需同步本机入口后通过同一检查。
- verify-spec-kit-sync.py增加核心入口及plan/tasks模板的Design binding标记校验；只证明副本/标记一致，不检查设计语义。
- 正向同步：11/11入口与个人shared文件hash PASS；临时复制入口移除implement标记后预期exit1，明确检出缺失标记；负例PASS。

## Concrete Design and Read-only Review

C-08包含12组CD、16组FIELD、10组FN、7条FLOW及PO，覆盖Runtime/缓存/请求/回调/会话/Provider/扩展/安装/CLI/binding。
使用CodeGraph精确node及canonical文件复核；宽同名结果可能来自staging，未作为源码权威。
独立只读audit_prepared_model核查existing签名与改法，发现并修正：

1. adapter目录与.pc.in路径错误，改为真实canonical目录。
2. Operation完整定义只在client TU；Access实现留同TU，Reader通过端口访问。
3. Package缺合作splitter来源；catalog typed构造分支显式保存新旧接口，T016前移到T003前。
4. clientFor缺冻结配置来源；Package保存精确registration，advanced参数规范化后同样冻结，不按模型名反查。
5. Conversation continuation无提交入口；private requestInternal注入native options，同步失败释放busy。
6. builtin同时继承两接口导致同名overload二义性；改为requestCooperative/planNativeRequestCooperative，底层仍共用一个impl，旧具体类型调用加入兼容编译反例。

最终限定只读复审确认上述六项闭合，无仍会直接阻断受审范围编码的已知契约缺口；C-08记录该基线的READY_FOR_IMPLEMENTATION，代码任务不勾选。

## Document Verification

- strict结构PASS：12 FR、7 SC、4 stories、16 tasks、0 complete；12需求可追踪。
- 16/16任务有Design binding；registry/卡片一致，T016先T003、T013先T012；315个本地Markdown链接存在。
- 同步脚本正向11/11及个人hash PASS；缺Design binding的临时implement入口被拒，预期负例PASS。
- authority索引刷新后Context Mode project/active health均exit0、ok=true；git diff --check通过。
- 双PDF三遍XeLaTeX完成：`.codex-tmp/design-pdf-20260912T093808890678Z`；current 91页、target 98页；provenance输入/输出hash一致。
- 无Overfull/Missing character/TeX error，字体嵌入；target新增84页已渲染目视检查，无裁切/重叠。
- 以上仅文档/技能检查，不是native build/test或完整Design源码baseline资格；生产仍NOT_RUN。

## Coverage Matrix

| Lane | Actual scope | Boundary |
| --- | --- | --- |
| production entry/callers | NativeInferenceClient/DI_NativeRequester、NativeInferenceProvider/Provider executable | 验证现有签名/owner及新调用接线，未运行新生产API |
| implementation/wire | catalog/preparation/planner/coordinator/runner headers及实际.cpp；C-08 FN/FIELD/FLOW | 不复制请求或durable状态机；新实现PLANNED |
| test/harness/oracle | tasks每项CD→FN→PO及C-04具名C++ selector | 新fixture/运行未实现，不算native PASS |
| build/source closure | 安装规则/ORT布局；同步脚本正负例；PDF构建/身份检查 | 原生install/ABI验证仍T015/T011 |
| migration/evidence | 16任务Design binding、C-07引用、T016顺序、技能安装副本 | 无新行政任务；历史证据不回写 |

## Readiness and Retrospective

**Closure decision**: CLOSED_FOR_VALIDATION（本轮文档/技能单元）；代码任务全部NOT_STARTED，下一T015。
**Batch growth decision**: 16任务11批不增加；T016在B1之后、B2之前，解决真实类型/构造依赖。
- static：上述具体签名/类型/接线缺口已修；详细设计审查与结构校验分开记录。
- compile-link：native NOT_RUN；安装头/ABI及具体builtin兼容反例已登记，不由本轮文档PASS替代。
- runtime-test：NOT_RUN；正负同步脚本用例是工作流验证，不是产品测试。
- unobserved：新代码实际行为/性能、竞争/清理及正式资格。

Design当前既有54文件漂移保留；未覆盖冻结API/源码快照。docs/failure-log.md与llm_pipeline_lib.py并行修改不纳入checkpoint。
