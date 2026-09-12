# API Audit and Standalone C++ Revision

**Status**: DOCUMENT_REVIEW_PASS / IMPLEMENTATION_NOT_STARTED
**Source baseline**: d484a047bb38a1a03978a11208d335af9842b36a；输入文件逐项SHA见[API inventory](api-inventory.json)。
**Scope**: 用户要求全API易用性审计，并明确C++完整独立入口、Python仅包装。只修改审计/设计/任务与清单生成器，不改生产实现、不编译native、不启动实验。

## Findings and Decisions

[API review](../api-review.md)列20项发现；[全文件索引](../api-surface-index.md)覆盖76项目C++头、139 Python模块、8500声明；1101 binding候选含共享Core TU，不冒充DI导出数量。语法解析无错误，宏/继承展开及全部声明的逐方法正确性不在已验证范围。
C-05统一模型key、能力查询、Result/DiError、时间单位、可靠流、扩展freeze及六层API暴露。
C-06规定配置/准备/请求/流/会话/恢复/Provider/异步清理全部在C++；T013完整C++资格先于T012。
T015安装/ABI、T016扩展边界加入原14项：共16任务11批，全部NOT_STARTED。
官方框架参照及原文链接在API review，属于工程取舍依据，不是可用性实验结论。

## Read-only Review Trace

沿用speckit-audit、speckit-plan和speckit-code-design文档流程；CodeGraph及真实源码对照。
独立只读审查audit_prepared_model检查C++安装/布局/owner/扩展调用方，再复审C-01/C-03/C-05/C-06及spec/plan/tasks/quickstart。
第一轮发现5项文档缺口：异步清理缺native接口、旧placement别名冲突、completion缺退订、B0 ASan标准不一致、exposure双权威路径。
修订为原生drainAsync、opaque策略句柄及合作端口、CompletionSubscription、统一normal+ASan和contracts/api-exposure.json。
第二轮只读复审确认上述5项闭合，未发现阻塞规划交付的新明显矛盾；不等于生产review-agent静态门或运行资格PASS。
后续每个实现任务仍须按C-04调用review-agent并保存实际skill hash/full diff/five lanes，不能复用本轮文档审查作实现证据。

## Verification

- inventory_api.py --check：PASS；76/139/8500/1101，与当前输入hash及generator/extractor一致，parse_errors为空。
- audit_speckit_structure.py --strict：PASS；12 FR、7 SC、4 stories、16 tasks、0 complete、12 traced requirements。
- 16/16任务卡与顶部registry一致，T013先于T012；排除代码块后全部本地Markdown链接存在，git diff --check通过。初始链接regex将C++ lambda误判为链接，修正检查器后重验。
- verify-spec-kit-sync.py --require-entrypoints：PASS，11/11。
- Context Mode authority重新索引；project/active health均exit 0且ok=true；权威仍是仓库文件。
- Design/build.py：双PDF三遍XeLaTeX完成；run `.codex-tmp/design-pdf-20260912T090511052656Z`。
- PDF provenance输入/输出hash一致；current 91页、target 98页；无Overfull/Missing character/TeX error，全部字体嵌入；新增C++内容在target 83–84页。
- target 83–84页已渲染并目视检查，无重叠/裁切，独立C++入口与任务顺序可读。
- 当前Design既有54文件源码漂移保留，未运行全baseline资格或覆盖冻结API快照；本次仅保证增量目标文档及构建身份。
- 全部native compile/runtime/sanitizer资格：NOT_RUN；上述测试矩阵均PLANNED。

## Boundaries and Next Unit

docs/failure-log.md和examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py已有并行修改不纳入本checkpoint；原始日志/构建目录不入Git。
声明清单初次误纳vendor及分析脚本Python版本差异已修正，最终清单重生成并--check；未据此重跑任何协议测试。
下一步T015：以最新source/diff核对安装闭包及ONNX布局，完成C++外部consumer normal/ASan门；之后按tasks依赖推进。Spec184未完成资格保持原owner。
