# 设计文档与 API 管理规则

本文件是可随 Git 交付的管理规则；仓库本机 AGENTS.md 引用并执行本文件。适用 NDNSF Core、NDNSF-UAV、NDNSF-DI、NDNSF-Repo 及其绑定。

## 文档职责

- 当前设计说明实际源码行为，包括尚未接通的接口和明确限制；目标设计说明用户接受的目标，不能凭 Spec 计划改写当前实现。
- 两份 PDF 的结构说明和 API 契约必须可编辑、可比较。R0/R1 初始化时一致，后续允许有意差异，禁止每次生成时自动覆盖目标文件。
- API 参考按 C++/Python/应用内部接口分层。语法清单负责准确签名和字段；中文契约负责行为、时序与错误，不能仅贴函数名或用自动扫描数宣称文档完备。
- spec-design-changes.md 关联 Spec 与设计变化；CHANGELOG.md 记录文档修订；Spec tasks/contracts 管理执行与验收；source-baseline.json 和 API 清单绑定源码身份。

## API 条目的最低要求

每个对外入口和关键扩展点应能查到：所属模块与 owner、完整签名与重载、参数类型/单位/默认值、返回值或 callback/event、调用前置条件、状态变更、错误/超时/取消、线程与资源生命周期、权限/版本绑定、调用时序、实现入口和验证证据。
公共数据类型必须说明关键字段、身份/摘要的区别、可选值和状态枚举。未声明或尚未验证的线程、异常、性能保证明确写“未声明/未验证”，不得猜测。
纯值访问器和同一契约的重载可共用行为说明，但每个签名仍须登记。测试 helper、内部接口、别名和兼容入口明确标记，不能混入已资格公开 API。

## 每个 Spec 的同步流程

1. 开始前读本规则、相关当前/目标章节和 API 参考，核对当前 Spec 指针及源码；登记 Spec 设计变更条目。没有设计影响时写 NO_DESIGN_CHANGE 和依据。
2. 设计阶段先写清目标 API 的前后差异、owner、兼容性和验证条件；用户只要求分析时不改代码、不改当前实现事实。
3. 实现阶段用 CodeGraph 定位入口/调用者，再核对实际源文件；API 增删、参数/默认值、状态、异常、线程、安全或数据格式改变都触发设计同步。
4. 重新生成当前 API 参考并审查差异，补中文行为契约；目标侧只按已接受设计更新，不自动覆盖。若生成器不覆盖宏、动态导出或继承方法，登记例外和人工核对入口。
5. 同步 spec-design-changes.md 的任务/契约、章节/API ID、前后行为、源码提交和证据；PARTIAL 不升级为 VERIFIED。完整历史回溯须逐条核对，禁止补造旧 Spec 记录。
6. 完成单元后同步 active tasks.md；失败保留独立原始目录并记录第一失败边界，再重试。文档构建检查、定向测试与运行资格分开记录。

## 验证与 Git

R2 起，当前 API 和目标 API 分别使用 api/inventory.json 与 api/target-inventory.json。
目标源码身份保存在 target-source-baseline.json 与独立补丁，target-snapshot.tex 不随当前刷新。
只有已接受的目标变更可以更新目标快照；禁止用当前清单覆盖目标。目标 TG-01 至 TG-05
是 PLANNED，当前实现与运行资格仍由源码、Spec 和证据决定。

源码范围由 design_state.py 定义：四模块维护源文件和配置，包含 .cpp 实现，排除测试、
vendor、模型和构建输出；另外保留已明确登记的支撑文件。新增/删除入口、实现或配置必须
检查集合差异。未跟踪的新产品文件先归入对应源码工作单元，不能仅靠 git ls-files 宣称覆盖。
快照范围不等于逐行语义审查；基线刷新必须保留未提交差异及实际资格状态。

当前更新顺序：build-api-reference.py → build-behavior-coverage.py → refresh-snapshot.py；
目标按需运行 render-api-contracts.py --target。检查 test_design_state.py、verify-api-reference.py
和两侧 verify-source-baseline.py 后，build.py 构建双 PDF，verify.py 核对同一次构建。
build-provenance.json 绑定所有 TeX/JSON/脚本/补丁输入与双 PDF；任何输入改变必须重建。
behavior-coverage.json 为每个函数登记 SIGNATURE_ONLY 或 CONTRACT_REFERENCED；不得把后者
解释为完整语义或运行验证。新工作单元应补行为缺口及验证证据，不能自动批量提升状态。

提交前检查源码摘要、API 签名与引用、当前/目标的预期差异、双 PDF 构建、文字/字体/分页/图表与链接；源码改变须运行适当测试，单纯文档修改不触发大型运行实验。
把 PDF、TeX、API 参考、生成/检查脚本、覆盖矩阵、Spec 追踪与精简证据作为同一文档单元提交到 Experimental。暂存显式路径或本单元 hunk，不能带入并行源码改动；不自动 push。
不提交原始日志、预览图片、源码压缩包、模型、密钥和构建缓存。AGENTS.md 若是本机忽略文件，只在本机维护规则入口，可交付内容以本文件为准，不为文档任务强行改变仓库指令文件追踪策略。

## 开发者指南写法

参考 NFD Developer’s Guide 的组织方式：先解释组件与数据结构，再讲处理流程、触发回调、允许动作、状态约束和扩展方法。使用 NDNSF 实际 API 和状态机，不能将 NFD 的转发接口直接套用为 NDNSF 接口。
每个 API 契约应使读者能定位实现、理解怎样调用或扩展，以及失败会在哪里发生；PDF 是可阅读的主线，完整声明参考是精确查询入口。

下一步按当前 Spec 的实际 API 改动执行上述流程，并逐步补齐已登记的历史设计映射。
