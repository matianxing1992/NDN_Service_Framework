# Complete API and Lifecycle Review

**Status**: DOCUMENT_REVIEW_PASS / IMPLEMENTATION_NOT_STARTED
**Source checkpoint**: c3ae00c7；本轮不改生产源码，不构建native，不做MiniNDN/SIF/Tiger实验。
**User intent**: 判断最终C++独立性、Python是否绑定、生命周期完整性及易用性，修订185并列出全部API。

## Scope and Findings

[C-07](../contracts/api-catalog.md)统一64编号条目（含重载/分组访问器）、公开值类型、C++→Python映射和生命周期矩阵；不是只列方法名，也不是把8500条内部声明全部承诺稳定。
既有[源码索引](../api-surface-index.md)覆盖当前76项目C++头、139 Python模块。inventory_api.py --check通过：8500声明、1101 binding候选、parse_errors为空；未变输入无需重新生成大清单。
主要修订：统一Subscription；resultAsync局部等待；observe/nextAsync可退订；read取消不吞事件；失败stream不伪装EOF；Runtime/handle/registration析构；明确Python直接pybind对象与便利适配层。
普通PrepareOptions移除取消callback，统一native handle取消；高级旧端口保持兼容。安装SDK及C++领域行为先验收，Python不得补缺。

## Review Trace

使用speckit-audit/code-design及plan契约审查；独立只读audit_prepared_model按C-01/C-02/C-03/C-05/C-06/C-07与tasks核对。
复审发现并修正：关闭executor后迟异步订阅无执行者；C-02遗留cancelled表述；64槽位按handle或operation定义不一致。
修正后close/stop拒新业务异步注册、既有结果同步可读；已经drained的drainAsync调用线程锁外完成，Python先构造Future；slot按全部副本共享且执行中引用不提前归还。
第二次只读复审确认三项闭合，未发现新增阻塞契约矛盾；仍须真实C++/binding用例验证竞态、GIL和清理。
文档审查不替代后续逐任务review-agent源码门，不计STATIC_PASS/行为PASS。

## Coverage and Verification

| Lane | Actual inspection | Boundary |
| --- | --- | --- |
| production entry/callers | CodeGraph explore及精确node NativeInferenceClient.hpp；源码result/observe实现 | 确认旧原生handle与新目标区别，未声称新API已存在 |
| implementation/wire | C-01至C-07参数、回调、关闭、流/commit边界；NativeInferenceClient.cpp:1608–1672 | 新目标不改Core wire或durable owner |
| test/harness/oracle | C-04新增生命周期反例映射T002/T004/T006/T009/T013；T012仅binding | PLANNED / NOT_RUN |
| build/source closure | inventory --check、结构/链接检查、双PDF身份/排版 | 文档与输入一致，不是安装SDK/ABI资格 |
| migration/evidence | tasks 16项及C-07每行任务归属，当前/目标分离 | T015→原生任务→T013→T012顺序不变 |

入口技能同步11/11 PASS；Context Mode project/active health均exit0、ok=true。
CodeGraph宽查询给出较旧短header片段，已用精确文件node与磁盘248行header核对；status显示up-to-date不足以替代源码核对，未据宽查询断言当前签名。
最新failure-log指向历史Provider binary缺失的启动前边界，本次仅核对原始default-path.log与后续证据，不重跑该实验。

### Document Results

- strict结构审计PASS：12 FR、7 SC、4 stories、16 tasks、0 complete、12 traced requirements。
- C-07 A01–A64连续唯一；tasks顶部registry与16任务卡一致，T013先于T012；290个本地Markdown链接无缺失。
- 技能入口同步11/11 PASS；authority索引刷新后project/active health均ok=true；git diff --check通过。
- 双PDF三遍XeLaTeX构建完成：`.codex-tmp/design-pdf-20260912T091908916160Z`；current 91页、target 98页。
- build provenance输入/输出hash一致；无Overfull/Missing character/TeX error、字体全部嵌入；target新增内容84页已渲染目视检查，无裁切/重叠。
- 这些结果仅支持本轮文档交付；完整源码baseline资格未运行，原生compile/runtime/sanitizer为NOT_RUN。

## Closure and Retrospective

**Closure decision**: CLOSED_FOR_VALIDATION（本轮文档单元）；新生产API仍NOT_STARTED，下一实现单元T015。
**Batch growth decision**: 补充职责归现有16任务/11批，不增加单独“审查/写测试/写报告”任务。
- static：发现上述生命周期/契约缺口并修订；CodeGraph宽片段需精确node核对。
- compile-link：native NOT_RUN；仅文档PDF构建单独记录。
- runtime-test：NOT_RUN；没有新安装/ABI/并发/行为资格。
- unobserved：全部新API实际实现、外部consumer及Python绑定；易用性未做用户研究。

当前Design既有54文件漂移保留，未覆盖冻结源码/API快照。docs/failure-log.md及llm_pipeline_lib.py并行修改不纳入本轮checkpoint。
