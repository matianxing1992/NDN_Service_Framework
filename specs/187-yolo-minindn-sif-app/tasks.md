# Tasks: YOLO MiniNDN SIF+APP Fast Path

**Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Status**: PARTIAL

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Candidate closure and pair mutation gate](#t001-candidate-closure-and-pair-mutation-gate) | PARTIAL | — | base smoke、APP SDK loader、完整 candidate build、容器 C++ unit 和 YOLO native runner 通过；host-gate、pair mutation 与完整 APP/配置闭包仍待执行；[candidate](evidence/b187-complete-candidate.md) | 2026-09-16 |
| [T002 C++ YOLO selector and MiniNDN caller wiring](#t002-c-yolo-selector-and-minindn-caller-wiring) | PARTIAL | T001 | C++ target compile/link、容器 C++ DI/YOLO unit、native runner 与当前依赖身份对齐后的 host through-MiniNDN terminal run 通过；维护 runner 的 MiniNDN 环境兼容层及回归测试已补齐；新增 C++ large-data segmented publisher 回归通过；T001 candidate-closure dependency 仍未闭合；[b187-local-yolo.md](evidence/b187-local-yolo.md)、[recheck](evidence/b187-local-yolo-recheck-20260916.md)、[large-data publisher](evidence/b187-large-data-publisher-cpp-20260916.md) | 2026-09-16 |
| [T003 Local YOLO pair build and two-run gate](#t003-local-yolo-pair-build-and-two-run-gate) | PARTIAL | T002 | 同一 candidate-bound config/input、同一 manifest 的两次当前依赖对齐 host C++/MiniNDN run 通过；regular base SIF/APP host-gate、同 pair SIF execution 与 mutation gate 仍待验收；[recheck](evidence/b187-local-yolo-recheck-20260916.md) | 2026-09-16 |
| [T004 TigerCluster same-candidate promotion](#t004-tigercluster-same-candidate-promotion) | WAITING_EXTERNAL_INPUT | T003 | T003 尚未 LOCAL_PASS；未启动 Tiger/Slurm；[b187-tiger-yolo.md](evidence/b187-tiger-yolo.md) | 2026-09-15 |
| [T005 QWEN deferral and delivery record](#t005-qwen-deferral-and-delivery-record) | DONE | — | QWEN 明确保持 TODO，未进入 YOLO candidate；[qwen-deferred.md](evidence/qwen-deferred.md) | 2026-09-15 15:16 -05:00 |
| [T006 Design-code convergence and final evidence](#t006-design-code-convergence-and-final-evidence) | PARTIAL | T001,T002 | 两层交付改变构建边界，需重新核对受影响调用与契约；历史静态 PASS 保留，不能覆盖新方案；[convergence-20260915-r1.md](evidence/convergence-20260915-r1.md) | 2026-09-15 |
| [T007 Pre-pack candidate closure gate](#t007-pre-pack-candidate-closure-gate) | PARTIAL | T001 | T007 static gate 通过且完整 candidate 已打包；真实 pre-pack C++ consumer 尚未前置执行，unit-r2 仅为 SIF 内 consumer 验收，需修正门禁或补前置证据；[candidate](evidence/b187-complete-candidate.md) | 2026-09-16 |

## Current Checkpoint

2026-09-17 RQ1 cost-model re-audit（独立文档任务，DOCUMENT_PASS）：
双语 §5.3 与 slides P20–21 已修正计数、固定 k 的增长阶、状态有效性和配对测试口径。
首轮讲稿引擎选择错误导致弯引号缺字，保留失败产物后恢复 pdfLaTeX，最终检查通过。
英文74页、中文57页、slides/PPTX53页；镜像、可编辑文字、LibreOffice回转与截图检查通过。
20项对照工具测试通过，103/134页对照稿完整覆盖349旧/346新单元。
证据：[cost-model re-audit](../../docs/PAPER/proposal-defense/cost-model-audit-20260917.md)。
T001–T007 及产品验收状态不变，没有新产品实验。原始证据目录：
`/tmp/ndnsf-cost-audit-20260917-leEAIK/`。只读checkpoint预检仍被既有全index hook阻止；
未改用户index、未绕过hook，无新commit或push，HEAD仍为`d5b241e6`。


2026-09-16 TigerCluster proposal plan（独立文档任务，DOCUMENT_PASS）：
双语DI正文及slides第35页增加正在准备的多节点GPU评价，明确不代表性能结果、
不替代UAV无线/移动测试。四入口编译、50页渲染和19项对照工具测试通过；
PPTX/LibreOffice与最终对照一致性通过；95页括号和123页左右版完整保留320新/349旧单元。证据：
[TigerCluster plan](../../docs/PAPER/proposal-defense/tiger-evaluation-plan-20260916.md)；
run：`/tmp/ndnsf-proposal-tiger-plan-20260916-vvOwy9/`。T001–T007不变，无产品实验。
本轮48路径已暂存；119个此前授权proposal待提交路径的普通checkpoint仍被全index
引用hook拒绝，首个命中`.specify/memory/constitution.md:16`；未绕过，无新commit或push，
HEAD为`d5b241e6`，日志为run的`checkpoint.log`。

2026-09-16 Skill-guided proposal review（独立文档任务，DOCUMENT_PASS）：
现有学术论证与NDN slides技能用于小幅修改，双语正文区分证据层次并预定比较标准；
slides仅改36、40页。四入口编译、50页slides/PPTX/讲稿/LibreOffice回转检查通过；
19项对照工具测试和319新/349旧单元完整性通过，括号94页/左右123页。
证据：[proposal skill review](../../docs/PAPER/proposal-defense/proposal-skill-review-20260916.md)；
run：`/tmp/ndnsf-proposal-skill-review-20260916-mDGrXA/`。T001–T007不变，无产品实验。
本轮50路径已暂存；117个既有授权proposal待提交路径的普通checkpoint被既有
全index hook拒绝，首个命中`.specify/memory/constitution.md:16`。未绕过或push，
无新commit，HEAD仍为`d5b241e6`；日志为run的`checkpoint.log`。

2026-09-16 Proposal language review（独立文档任务，DOCUMENT_PASS）：
双语正文及22页slides文字已修订，保留自然proposal语气和必要限制；研究结构及历史数字不变。
四入口论文（英文65／中文51页）、50页slides／讲稿／PPTX／LibreOffice回转检查通过。
固定图表裁剪已改为按当前几何定位；11+8项工具测试通过，对照完整保留318新／349旧单元，
金色括号版94页、左右版123页；讲稿alias已纠正。证据：
[language review](../../docs/PAPER/proposal-defense/language-review-20260916.md)，
run：`/tmp/ndnsf-proposal-language-20260916-N4R7eE/`。T001–T007状态不变。
59路径已暂存；普通checkpoint被既有全index hook拒绝，首个命中
`.specify/memory/constitution.md:16`，未绕过、无新commit；HEAD仍为`d5b241e6`。
日志为run的`checkpoint.log`；本文件保留并行修改，不整体暂存。

2026-09-16 Proposal register review（独立文档任务，DOCUMENT_PASS）：
在现有结构内说明三个RQ的研究答案／判断依据、已有协作基础及UAV/DI复用检验；
双语论文同步，slides仅改第3、8、31、32、40、41、43页。摘要、RQ定义、历史测量及时间线不变。
四入口编译、50页slides／PPTX／讲稿／LibreOffice回转、19项对照工具测试通过；
95页括号版和121页左右版保留317新／349旧单元。证据：
[proposal register review](../../docs/PAPER/proposal-defense/proposal-register-review-20260916.md)。
原始run：`/tmp/ndnsf-proposal-register-20260916-532JHS/`；T001–T007状态不变，无产品实验。
57路径已暂存；普通checkpoint因既有全index hook失败，首个命中
`.specify/memory/constitution.md:16`，未绕过、无新commit；日志为run的`checkpoint.log`。


2026-09-16 Latest-design proposal synchronization（独立文档任务，DOCUMENT_PASS）：
双语论文同步Core提交检查、DI原生PreparedModel入口及ACK关闭后规划顺序、UAV三层职责；
slides仅修改第25、31、32、33页。四入口论文、50页slides及讲稿、PPTX可编辑文字／
LibreOffice回转检查通过；95页括号版与120页左右版保留316新／349旧单元，19项工具测试通过。
证据：[design sync](../../docs/PAPER/proposal-defense/design-sync-20260916.md)。
原始run：`/tmp/ndnsf-design-sync-20260916-oD8E9i/`；只读检查器备份路径错误已修正。
T001–T007状态不变，无新产品实验或资格结论；本文件保留并行修改，不整体暂存。
53个本轮文档路径已暂存；普通checkpoint被既有全index hook拒绝，首个命中
`.specify/memory/constitution.md:16`；未绕过、无新commit，详见run的`checkpoint.log`。


2026-09-16 Proposal/slides audit synchronization（独立文档任务，DOCUMENT_PASS）：
参照reviewed PPTX的26条意见，修订双语需求验证句与slides第5、34、43页；
47页frame及实验数字不变。四入口论文构建／镜像文字、50页slides／讲稿、
PPTX可编辑文字及LibreOffice回转检查通过；金色括号版94页、左右版117页
保留349旧／307新单元，11+8工具测试通过。首轮讲稿路径及检查器titlepage
计数失败已修复，原始输出保留于`/tmp/ndnsf-paper-slides-sync-20260916-AsSp5o/`。
详见[audit sync](../../docs/PAPER/proposal-defense/audit-sync-20260916.md)。
T001–T007状态不变；无产品代码／实验，科学结论和完整性仍需研究证据。
48路径已暂存，普通checkpoint被既有全index引用hook拒绝，
首个命中`.specify/memory/constitution.md:16`；`checkpoint-r2.log`保留。
未绕过hook，无新commit；本文件与failure-log含并行修改，不整体暂存。


2026-09-16 Proposal necessity review（独立文档任务，DOCUMENT_PASS）：复核95项既有变更的
必要性，区分事实／批注所需修正、编辑选择及条件性研究方案；局部修正双语
需求模块的签名／依赖措辞、计划时态及重复说明。四入口构建、镜像文字、摘要
和目录标题保留、115页文字边界及修改页视觉检查通过；11+8工具测试通过，
94页括号版与117页左右版保留349旧／307新单元，文字／颜色／顺序／边界检查通过。
95项分类无遗漏无重复，但不声称每个措辞都是唯一必需选择或科研结论全部成立。
39路径普通checkpoint被既有全index引用hook拒绝，首个命中
`.specify/memory/constitution.md:16`；原始目录checkpoint.log保留。
无新commit、未绕过hook；交付保存并暂存。
见[necessity review](../../docs/PAPER/proposal-defense/necessity-review-20260916.md)。
不改变T001–T007状态，未运行实验；本文件的并行修改不整体暂存。

2026-09-16 Proposal R1/R2 targeted revision（独立文档任务，DOCUMENT_PASS）：已补中英文
需求—失败—验证追踪表及风险措辞；四入口构建、镜像／边界／修改页视觉检查通过。
中文交叉引用及对照固定页号裁剪已修复；11+8工具测试通过，94页括号版及117页
左右版保留全部349旧／304新单元。P18/P19为TEXT_ADDRESSED，不代表研究验收。
原始失败输出保留，42路径普通checkpoint被既有全index引用hook拒绝，
首个命中`.specify/memory/constitution.md:16`；checkpoint-r3.log保留，
无新commit、未绕过，交付已保存并暂存；
见[targeted revision](../../docs/PAPER/proposal-defense/comment-fixes-20260916.md)。
不改变T001–T007产品状态，未运行新实验；本文件的并行修改不整体暂存。

2026-09-16 Proposal advisor-comment re-review（独立审计，MINOR_REVISION）：查看老师
`proposal 2.pdf`全部11页并复核19条批注；17条表达要求已回应，P18/P19的需求
推导和范围内覆盖仍为PARTIALLY_ADDRESSED。95项实质变更均有理由映射，另发现
风险表compute savings残留；正文、slides、对照PDF和实验数据未修改。
源哈希／19批注／95项无遗漏无重复检查通过；报告和限制见
[comment-change audit](../../docs/PAPER/proposal-defense/comment-change-audit-20260916.md)。
两个审计文件已暂存；普通限定路径checkpoint被既有全index hook拒绝，原始目录
checkpoint.log保留，无新commit、未绕过。不改变T001–T007状态；本文件并行修改不整体暂存。

2026-09-16 Proposal inline comparison（独立文档任务，DOCUMENT_PASS）：按用户要求制作
新版正文黑色、括号内旧文金色的单栏论文对照。93页构建和8项工具测试通过；
验证器计数错误已修复，300新版/349旧版单元的颜色通道保留、顺序、边界及视觉
检查通过。未修改正式论文或实验；旧左右对照另存；限定34路径checkpoint被既有
全index hook拒绝，无新commit、未绕过；交付文件保存并暂存，checkpoint.log保留。
见[inline review](../../docs/PAPER/proposal-defense/inline-comparison-review-20260916.md)。
不改变T001–T007状态，本文件的并行修改不整体暂存。

2026-09-16 Proposal semantic comparison（独立文档任务，DOCUMENT_PASS）：逐主题审阅
Origin→当前稿修改并作中英同步修正；四入口编译及9项对照工具测试通过。
首次镜像文字一致性检查发现NSC书目副本未同步，已修复；根/镜像文字一致。
英文63页、中文50页及对照116页完成渲染、边界/保留性和视觉检查；649源单元完整。
研究结构、实验文字/数值和16份slides文件不变；限定28路径checkpoint被既有全index
hook拒绝，未绕过、无新commit；交付文件保存并暂存，原始checkpoint.log保留。
见[semantic review](../../docs/PAPER/proposal-defense/semantic-comparison-review-20260916.md)。
不改变T001–T007产品验收状态；不整体暂存本文件的并行修改。

2026-09-16 Proposal paragraph comparison（独立文档任务，DOCUMENT_PASS）：以完整59页Origin
和当前63页main.pdf重建114页逐段/段落组对照。644源单元均保留，69个人工主题锚点；
正文字符与顺序、65/70标题、逐框文字/页面边界、7/7工具测试及全页渲染检查通过。
工具类型、截图高度、隐藏文字及表格零高度线边界均已修复，原迭代记录保留。
见[comparison review](../../docs/PAPER/proposal-defense/paragraph-comparison-review-20260916.md)。
不改变T001–T007产品验收状态；本文件含并行工作，不整体暂存。
限定6路径checkpoint被既有全index hook拒绝，无新commit、未绕过；交付文件保存并暂存。

2026-09-16 Proposal slide synchronization（独立文档任务，文档验收PASS）：按最新论文结构
修订同源50页deck（44页主体），补研究假设、验证阶段和工作包；数字不变。
四份PDF/notes构建通过；1021/1021文字片段、717个可编辑文本框、50页notes。
LibreOffice全页回渲、文字/边界及修改页放大检查通过；普通内容最多94词。
首次PPTX目录拒绝、P4/P29溢出和表前段落间距问题均保留日志并修复，非产品失败。
原始日志及持久记录见
[slide sync](../../docs/PAPER/proposal-defense/slides/research-structure-sync-20260916.md)。
不改变产品/API、T001–T007状态，不运行新实验；本文件含并行进度，不整体暂存。
限定11个slide路径的普通checkpoint仍被既有全index hook拒绝；无新commit，文件暂存待提交，未绕过。

2026-09-16 Proposal change-value review（独立文档任务）：对照完整59页Origin，完成
65个目录项、摘要、5组图、10张表和14项新增内容的95单元评价；中英文同步修正引言、
研究缺口/假设、相关工作分类、UAV/DI细节和风险。保留七个主章，旧数值及其限制移入历史附录。
四入口编译通过，English63页/Chinese50页；PDF文本镜像一致、日志及边界检查通过，
全页缩略图与附录放大图已审查。三RQ、已修正摘要和第5章当前实验数值保持不变；
未运行新实验，slides/产品/API及T001–T007状态不变。详见
[change-value review](../../docs/PAPER/proposal-defense/change-value-review-20260916.md)
及[focused validation](../../docs/PAPER/proposal-defense/change-value-validation.json)。
普通checkpoint再次被既有全index提交hook拒绝，38个论文路径暂存待提交；没有新commit，
未绕过。本文件含并行进度，不整体暂存，日志与首个失败边界见上述review的Checkpoint Boundary。

2026-09-16 B187-LOCAL-YOLO current-source replay：在修复 exact-SIF 输出根校验并经
官方 `review-agent` `STATIC_PASS` 后，当前源码受影响 Native/DI 目标以 `-j4`
重链成功，native receipt 返回 `SPEC180_NATIVE_IDENTITY_OK`。全新 root-owned
run-id `spec187-local-yolo-1789592534950657908` 的本机 MiniNDN Y-A 返回
`SPEC180_CASE_RESULT status=PASS`，C++ 日志关联 `ACK_CLOSED`、
`SELECTION_COMMITTED`、`SELECTION_ACCEPTED`、`PROVIDER_EXECUTION_COMPLETED`、
`SPEC187_NATIVE_REQUEST_PASS`，并观察到 1,601 个分段输入、7,267-byte 终端结果和
完整子进程清理。r52/r53 的非空输出根拒绝、Waf 配置/链接失败均保留原始记录；
本轮仍未启动 SIF/APP、negative path 或 Tiger，T001/T003/T007 保持 `PARTIAL`，
T004 保持 `WAITING_EXTERNAL_INPUT`。详见 [current-source replay](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-current-source-local-yolo-replay-after-segmented-input-changes)。

2026-09-16 B187-LOCAL-YOLO corrected-dependency replay：复用同一候选输入并将
`LD_LIBRARY_PATH` 固定到已验证的 `ndn-svs/build-spec187-local` 依赖身份，新的
root-owned run `spec187-local-yolo-1789593707473878369` 返回
`SPEC180_CASE_RESULT status=PASS`。结果为
`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，观察到 ACK closed、selection
commit/accept、Provider execution completed、1,601 个分段输入和 7,267-byte
终端结果；子进程完成清理。原始记录和输出见 [corrected replay](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-corrected-dependency-local-yolo-replay)。
本轮仍未运行 SIF/APP、negative path 或 Tiger，T001/T003/T007 保持 `PARTIAL`，
T004 保持 `WAITING_EXTERNAL_INPUT`。

2026-09-16 Abstract confidentiality/authorization wording（独立小修）：按用户反馈拆开发现
描述保密与调用授权说明，四个中英入口同步构建通过、同语言PDF文本一致。
正文及产品状态不变，未运行实验；记录见
[abstract review](../../docs/PAPER/proposal-defense/abstract-review-20260916.md)。
保留既有提交钩子阻塞，不计作T001–T007产品进度。

2026-09-16 Proposal coverage restoration（独立文档任务）：对照完整59页Origin，补回
NDN/中间件机制、研究交付物、ACK选择/准入、UAV完整流程、DI规划依据、历史观察及
阶段验证/工作包/风险。中英文目录同步，四入口编译通过；English63页、Chinese49页，
同语言根/子目录PDF文本一致，无未定义引用/Overfull/缺字/文字越界。
正文保留与新增模块检查、全页缩略图及新表格放大审查见
[coverage restoration](../../docs/PAPER/proposal-defense/coverage-restoration-20260916.md)。
原实验数值不变，无新产品实验；slides未修改；T001–T007及产品/API状态不变。
本轮普通commit仍被既有全index扫描拒绝，日志及33个文档暂存范围见上述记录；未绕过。
本文件含并行进度改动未整体暂存，不将文档验收当作产品资格完成。

2026-09-16 Proposal abstract revision（独立文档任务）：对照完整 Origin 及导师批注稿，
恢复总体应用动机、框架接口/消息、调用/数据处理覆盖和两套应用的验证细节；同步中英摘要。
三入口编译通过，英文 PDF 文本一致，摘要外源码不变，摘要页已渲染检查。
详见 [abstract review](../../docs/PAPER/proposal-defense/abstract-review-20260916.md)。
未运行实验，T001–T007 与产品/API 状态不变；旧 comparison PDF 保留为冻结快照。
普通本地提交被既有 pre-commit 全 index 引用扫描拒绝；未绕过，论文文件待提交。

2026-09-16 UAV fixed-camera clarification（独立文档任务）：P3 标定术语对齐固定相机，
P4 将云台标为后续备选，新增 P5 固定相机方案及限制；双 PDF 与可编辑 PPTX 同步为 5 页。
双遍 LaTeX、文字预检、117/117 spans、78 editable textboxes、5 hyperlinks 与
LibreOffice/Poppler 全页渲染检查通过；详见
[fixed-camera review](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md#fixed-camera-baseline--2026-09-16)。
产品/API 与 T001–T007 状态不变，无新实验；保留既有提交钩子阻塞，未绕过。

2026-09-16 UAV slides editable export（独立文档任务）：新增四页
`docs/NDNSF-UAV/slides/UPDATES_UAV.pptx` 与复用 proposal exporter 的 wrapper。
99/99 source spans、66 editable textboxes、4 independent figures、4 hyperlinks
及画布边界检查通过；LibreOffice/Poppler 全页渲染核对通过。
详见 [UAV export review](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md#editable-powerpoint-export--2026-09-16)。
原 PDF/TeX、产品/API 与本 Spec T001–T007 状态不变；未构建 SIF 或运行实验。
Checkpoint commit 被现有 pre-commit 的全 index 开发辅助引用扫描拒绝；未绕过钩子。
交付文件保留待提交，文档验证 PASS 不受影响，详见上述 review 的 Checkpoint 记录。

2026-09-16 B187-LOCAL-YOLO segmentation repair and host verification：修复大输入
单 Data 发布边界，6,555,271-byte YOLO input 现在按 4,096-byte encrypted NDN
segments 发布，共 1,601 段；Provider 使用 SegmentFetcher 组装后再解密和派发。
C++ selector JSON evidence matcher 与 MiniNDN Boost.Test `--color_output=no`
经过两次 official review-agent `STATIC_PASS`，selector 以 `-j4` 编译 30.668s，
native identity manifest 以 `SPEC180_NATIVE_IDENTITY_OK` 通过。使用同一
candidate-bound native config/input 的两次全新 host MiniNDN Y-A run 均为
`SPEC180_CASE_RESULT status=PASS`（39.62s、40.17s），结果各 7,267 bytes。
此前 single-Data、selector ANSI false-negative 及 envelope-key preflight 失败
均保留原始日志；SIF/APP host-gate、same-pair SIF execution、T004 Tiger promotion
仍未执行，任务不提前标为完成。

2026-09-16 B187-LOCAL-YOLO maintained runner repair and recheck：旧 MiniNDN
`popenGetEnv()` 对包含 `=` 的环境值解析不健壮，维护 runner 新增进程内兼容层，
并以回归测试覆盖 `util.popenGetEnv` 与实际 `application.getPopen` 两条入口。
官方只读复审返回 `STATIC_PASS`；指定兼容/门禁/副作用测试 3/3 通过。r39 使用
维护脚本、同一 candidate-bound config/input、现有 `build-spec187-local-nac-r1`
和新的 root-owned 状态/输出目录完成 host MiniNDN Y-A，terminal response、
7,267-byte native result、`SPEC187_NATIVE_REQUEST_PASS` 及子进程清理均已观察。
整份旧测试文件仍有一个与本修复无关的既有哑对象夹具失败，未把它计入本批 PASS；
SIF/APP host-gate、same-pair SIF execution、T004 Tiger promotion 仍未执行。
详见 [recheck](evidence/b187-local-yolo-recheck-20260916.md)。

2026-09-16 B187-LOCAL-YOLO current-source replay：native receipt 以系统优先库路径
重新生成并通过 `SPEC180_NATIVE_IDENTITY_OK`；r40/r41 的输出与身份前置门禁均保留，
r42 在 Controller 控制阶段以 `corrupted size vs. prev_size` abort，未进入协议请求链。
因此不增加当前 MiniNDN PASS 次数，也不把 r42 计为失败协议结果；T002/T003 仍以
r27/r28/r39 的 host Y-A terminal PASS 为历史可复用证据，当前源码重放待 native
崩溃最小化诊断和复测。SIF/Tiger 仍暂停。

2026-09-16 C++ segmented input regression：按 ndn-cxx
`prefix/version/segment` 契约在 request-scoped input 基础名加入由 attempt 派生的
Version component，并同步 User/Provider AAD 与 fetch name。官方静态复审快照
`review-segmented-input-20260916-r3` 返回 `STATIC_PASS`（changes SHA256
`1ca6d5017d0ffd0d8990bbd23dfb033fcc7cb1122432c0ca49af5042c210e020`）。既有
`build-spec187-local-nac-r1` 以 `-j4` 重建 integration target 成功；C++ selector
`RequestScopedSelection/*`（4 cases）和 `RequestScopedResponseConfidentiality/*`
（4 cases）第二次整套运行均 `rc=0`。新增长输入 case 真实观察
6,555,271 bytes→1,601 signed segments、统一 FinalBlock、最大 wire <8,800 bytes，
Provider SegmentFetcher 完整组装后才进入 handler；证据见
[segmented regression](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-c-segmented-request-regression)。
首次 suite timeout 原始记录保留，未计入 PASS。该 C++ DummyFace 回归修复并证明
分段传输边界，但当前 MiniNDN r42 Controller native 崩溃仍未修复，T002/T003 继续
PARTIAL，不外推为当前源码 MiniNDN 或 SIF/Tiger qualification。

最小化诊断补充：相同环境下 `ServiceController` 构造成功，但显式删除 native 对象
稳定 `SIGSEGV`，GDB 位于 Certificate map 析构路径；尚未把该现象归因到具体依赖或
提交修复，不能用强制进程退出替代正常生命周期验收。

2026-09-16 B187-LOCAL-YOLO dependency-aligned replay：定位并修复本机 Waf 选择的
NDN-SVS 头文件/二进制不一致（`subscribeToProducerWithCatchUp` undefined symbol）。
以源码 `9f2d8a4` 和本机 Boost 1.71/ndn-cxx 重新构建 SVS，重配置并以 `-j4`
完成 Spec187 受影响目标 `205/205`；native identity verify 通过，实际映射不再
落到 `/usr/local`。在全新 root-owned state/output 目录中，当前源码 YOLO `Y-A`
连续两次返回 `SPEC180_CASE_RESULT status=PASS`，C++ selector 均写出
`SPEC187_NATIVE_REQUEST_PASS`，subcase 为
`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`，native result 各 7,267
bytes，子进程清理完成。持久记录见
[dependency-aligned replay](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-csvs-dependency-aligned-minindn-replay)。
本轮只闭合当前源码的 host MiniNDN 正向链；SIF/APP host-gate、同 pair SIF
execution、MiniNDN negative path 和 Tiger 仍未执行，T001/T003/T007 继续
`PARTIAL`，T004 继续 `WAITING_EXTERNAL_INPUT`。

2026-09-16 B187-LOCAL-YOLO current-source confirmation：r50 的两个启动边界
（`STATE_ROOT_OWNER_MISMATCH`、最小环境缺少 `SHELL`）均在协议前被保留；补齐
root-owned 目录和完整运行环境后，r51 在同一 candidate-bound config/input 与
匹配依赖上完成 Y-A，退出码 0，终端响应验证通过，native result 7,267 bytes，
子进程清理完成。该结果加强本机 MiniNDN local PASS，但不改变 T002/T003 的
`PARTIAL`，因为 regular base SIF/APP host-gate、SIF execution、negative path
和 Tiger 仍未执行；详见
[r50/r51 evidence](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-current-source-local-miniindn-confirmation)。

本轮文档与证据组合审查使用不可变快照
`.codex-tmp/review-spec187-local-yolo-r48-r49-20260916/`，5 个快照文件校验通过，
官方 `review-agent` 返回 `STATIC_PASS`，无 P0–P3；审查确认上述状态边界未越界。

2026-09-16 request-scoped segmented-input selector 复核：第一次直接运行因命令的
`LD_LIBRARY_PATH` 选中了旧 NDN-SVS build，在夹具请求发布边界失败；`ldd` 与原始
日志保留于 `.codex-tmp/spec187-segmented-input-r1/`。将匹配的
`/home/tianxing/NDN/ndn-svs/build-spec187-local` 置于搜索路径首位后，同一 C++
selector 返回 `rc=0`、`No errors detected`；详见
[segmented selector rerun](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-request-scoped-segmented-input-selector-rerun)。
该结果确认输入分段实现仍通过，不改变 T001/T002/T003 的总体 `PARTIAL` 及 SIF/Tiger
未执行边界。

2026-09-16 清理了可重建的旧构建产物和未占用的旧 Codex 会话，磁盘恢复约 43 GiB；r12 诊断 SIF/rootfs 已移除，失败日志和证据保留。T007 冻结审查返回 `STATIC_PASS`，14 个模板测试、2 个 header/NDNSD 子集测试及 Python/`bash -n` 通过；已建立干净 HEAD `a0740640` worktree。

2026-09-16 authority 闭包重建：source handoff 从干净 worktree 更新至 `bd5b2f3e`，新候选 `ec21657d…fc1eb` 完整构建 `295/295`，cleanenv native verifier、C++ DI unit 和 C++ YOLO native runner 均通过；构建记录仍为 `BUILT_UNQUALIFIED`。候选-bound authority 配置、through-MiniNDN 两次请求、host-gate/APP pair 与 Tiger 仍未执行，T001/T002/T003/T007 保持 `PARTIAL`。

2026-09-16 B187-AUTHORITY-CLOSURE 静态/脚本批次：`DI_NativeArtifactAuthority` 已接入 source seal、Waf、builder/final 安装和 ELF/ldd/manifest 门，并同步 APP/validator 与 fixture。官方 review-agent 冻结快照 `review-authority-r2-20260916` 返回 `STATIC_PASS`（SHA-256 `36f3b9d7…de8d07`，五 lane 无 P0–P3）；定向测试 39 passed、1 skipped（4.16s）。尚未重建新候选，T001/T003/T007 继续 `PARTIAL`。

2026-09-16 完整两阶段 candidate SIF 已生成：Apptainer 1.5.3，SIF SHA-256 `e6cef05a949c3b865b35424ddb486bee05ea8a0023dd9ba4f7f5eda556541657`，Waf C++ 293/293、两个 Python binding wheel、builder/final `verify-native.py`、Python import、`ldd` 和 SDK 4278 项检查通过；构建记录为 `BUILT_UNQUALIFIED`，不代表行为资格。容器临时 rootfs 已在保留记录后清理。

2026-09-16 `unit-r1` 首次运行在候选库加载和测试编译后，于 C++ fixture 创建 `/home/tianxing/.ndn` 时因 `--containall --no-mount home` 不可写而返回 134；原始失败记录保留。run-di-unit-smoke.py 补充每次独立的 `0700` HOME、owner/目录校验和显式 bind，官方 review-agent 复审 `STATIC_PASS`。`unit-r2` 在同一 SIF、source revision `a0740640` 下容器内编译并运行 4 组 C++ DI/YOLO native tests，通过 `DI_CPP_UNIT_SMOKE_PASS`，二进制 SHA `b724eb792b9812bb8c76d48fcee4de68c30a87619ec30a8a136a28a533af721a`；记录 `.codex-tmp/spec187-clean-restart/unit-r2/`。

2026-09-16 `yolo-r1` 首次运行同样因候选隔离 HOME 不可写返回 134，ORT 与 NDNSF-DI ELF 已加载；失败记录保留。run-yolo-cpu-smoke.py 采用同一 `0700` HOME 修复并经官方 review-agent `STATIC_PASS`；`yolo-r2 --native-runner` 在候选 SIF 内编译/运行 ORT+C++ tensor codec/runner 三次，50 行输出每次最大绝对误差 `0.000534058`，通过 `YOLO_CPU_NATIVE_RUNNER_PASS`，二进制 SHA `f2d5d1cd6eddef9abec17829c63e13e7efde6da541c90b5bef05ca8f467703fd`；记录 `.codex-tmp/spec187-clean-restart/yolo-r2/`。

当前仍未完成：host-gate/APP pair mutation、同一 pair 的 SIF execution、MiniNDN
负路径及 Tiger promotion；host MiniNDN Y-A 的正向 terminal chain 已由 r27、r28
和维护脚本 r39 观察并留有证据。T001/T002/T003/T007 保持 `PARTIAL`，不能把
host local PASS 或 unit/YOLO smoke 外推为 SIF 或 Tiger PASS。

2026-09-16 流程已重排为端到端候选门：source closure → container build → pre-pack C++ consumer → SIF packing → cleanenv native verifier → C++ unit → YOLO → MiniNDN。T007 先修复并验证 assembled runtime tree，未通过前不再封装；现有 r12 仅作诊断候选，不计 T001/T003 完成。

2026-09-16 unit-r2 暴露 candidate 安装 DI headers 仍为父镜像旧版；r12 不是可交付候选。正在补齐真实安装清单和 header/source 一致性门，只重封装已有原生二进制，C++ 单元与 YOLO 仍待通过，T001/T003 PARTIAL。

2026-09-16 r12 最终 SIF 与隔离 native 加载 PASS，摘要 `c786bed8…`，receipt 为 BUILT_UNQUALIFIED；开始容器 C++ 单元验收。YOLO/MiniNDN/Tiger 尚未通过，T001/T003 仍 PARTIAL。

2026-09-16 r11 封装复制因缺少 fakeroot 无法读取三个容器私有运行目录，已中止并保留失败日志；r12 已补权限映射重试，原 final rootfs 与生产库保持不变。4 个恢复入口回归通过；最终镜像和模型验收仍未完成，T001/T003 PARTIAL。

2026-09-16 r11：base SDK 4278 项及实际编译 probe PASS，修复后的默认隔离 native verifier exit 0，已进入最终 SIF 封装；未重编 NDNSF。17 个脚本回归通过。最终 SIF 单元/YOLO 尚未运行，T001/T003 保持 PARTIAL，见 [candidate](evidence/b187-complete-candidate.md)。

2026-09-16 r9 默认容器加载失败已定位为继承 SDK 环境覆盖项目库目录。正在审查 92 环境修复及最终 SIF 默认环境门；仅恢复已完成的 r8 final rootfs，不重编生产库。用户明确最终镜像须自包含，不依赖宿主源码/库/home；测试仅挂载声明的测试输入和输出。T001/T003 保持 PARTIAL，C++ 单元与 YOLO 实测尚未通过。

2026-09-15 标准 r8 的完整 builder/final post 通过，但最终 SIF copy 因磁盘峰值不足失败，当前无有效候选。保留 final rootfs；只恢复封装并复验身份/ABI，不重编 Core/DI。最终镜像、容器单元、YOLO 仍未验收，T001/T003 PARTIAL。

2026-09-15 标准 r8 正在构建：正常两阶段入口/SDK/configure 已通过，C++ `-j4` 进行中。此前恢复产物已归档验证；不采用被拒绝的 final 恢复路径。容器单元 runner 与文档静态/组合通过，实际单元/YOLO 与最终 SIF 仍待验收。

2026-09-15 r7：Repo binding 与 builder ABI/import 检查通过；恢复 final definition 被正常交付门禁拒绝宿主二进制输入，尚无最终 SIF。配置根本修复已提交 `040c2dfd`，25 tests passed。下一步用修正后的正式两阶段模板重建 NDNSF 层、复用封存 base，再验收容器单元/YOLO；不放宽交付门禁。

2026-09-15 r6：Core/DI C++ 293 steps 与主 Python binding 通过；Repo binding metadata 在显式库目录契约处失败。正在修复 template 调用参数并准备从该步骤恢复；最终候选及容器内 C++ 单元/YOLO 验收仍未通过，T001/T003 保持 PARTIAL。

2026-09-15 source closure checkpoint `016daa38`，13 tests passed；恢复准备静态/组合通过，已复用 r3 rootfs/configure/Rust 继续原 C++ `-j4` 构建。最终 SIF、容器内 C++ 定向单元测试和 YOLO runner 尚待完成，T001/T003 保持 PARTIAL。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL：build-r3 的 SDK/configure/Rust 已通过，Waf 因源码归档遗漏 DI `.pc.in` 模板停止，尚未开始 C++ 编译。原始日志和 rootfs 保留；修复封存清单并静态复审后复用现场。未生成候选或推理 PASS；见 [candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 B187-CONSUMER-SCRATCH 静态/组合审查与 25 个定向测试通过（3.75s）；开始新 bundle-r5 的候选重试，实际原生编译/验收仍 PARTIAL。修改只影响容器 scratch 与失败保留，不改变封存 base。

2026-09-15 B187-CONSUMER-SCRATCH / PARTIAL：已恢复，修复容器临时目录隔离，r1 静态/组合通过；补充失败现场保留的 r2 增量审查后统一测试，再生成新 definition 重试。封存 base 不变；[candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 storage cleanup DONE；候选构建仍暂停：已归档并逐文件核对 9 月 6 日两个旧 native build 后释放原目录；旧临时 checkout 的 RELEASE 副本与保留件比较一致后删除。Codex 两份故障备份无损压缩；82 个超过 30 天未更新的会话经 archive 内容比较及活动检查后压缩归档，可恢复，未改数据库。当前可用约 23 GiB；封存 base、模型、密钥和当前/近期会话保留。下一步先修复 build-r2 的临时目录权限/隔离边界，再继续构建。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL，按用户要求先暂停构建、清理磁盘：build-r2 已通过 base SDK 复验，随后在清理旧 `/tmp/nac-abe-build` 等目录时因权限拒绝停止，尚未编译仓库目标。原始 log/record 保留；清理完成后先修复构建临时目录隔离，不重建或修改封存 base。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL：新 `bundle-r4` 与 definition 已封存，使用不可变 base `8ebfc464…` 启动完整候选 build-r2。只重建仓库目标，外部 SDK 沿用 base；原生 runner r2 已静态/组合通过，待本轮候选内实际编译运行。原始日志 `.codex-tmp/spec187-app-build-20260915/build-r2/build.log`；构建中不计 PASS，见 [candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / DONE（base only）：最终 SIF SHA-256 `8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9`，4,087,824,384 bytes。最终镜像基础/SDK 原生检查通过，坏库覆盖按摘要拒绝；C++/ORT YOLO CPU 三次 oracle 对照通过。已封存于仓库外 `ndnsf-artifacts/base-sif/<sha256>/`，完整清单校验与移动后镜像复验通过、文件只读；构建 lock 已绑定新 base。T001/T003 保持 PARTIAL，下一步是 NDNSF consumer 实际构建与本地完整候选验收；不声明 MiniNDN/Tiger。见 [封存证据](evidence/b187-base-sdk-sealed.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：r5 修复后已在保留 rootfs 通过真实 SDK C++/Rust/Python/GStreamer/ELF 验证及基础 NumPy/NFD smoke；consumer r2 静态门与 46 项定向测试通过（5.59s）。当前封装最终 base SIF，按用户要求先验收并永久封存，再继续 NDNSF candidate；尚不声明最终镜像 PASS。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：首次增建已编完外部库，但 C++ probe 缺 NAC include 子目录而失败；保留 FAIL record 与 rootfs，修正并复审后在已有构建现场复验。T001/T003 不计完成；[失败边界](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：r4 不可变快照通过只读 `STATIC_PASS / B187-BASE-SDK_COMPOSITION_PASS`；`test_dependency_sdk.py` 与 `test_base_runtime.py` 共 24 passed（1.19s）。正在从现有 base 增建 `images/base-sdk-20260915-r1`，实际 SDK 验收尚未完成；随后接入 NDNSF consumer。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：用户接受 `base SIF + NDNSF` 两层，并要求从现有 base 增建依赖。脚本新增同一 base 入口的 dependency-bundle 模式，待静态门和真实构建验收；NDNSF consumer 下一批接线。本机 itiger-ndnsf-ops 安装版原为 2932 行过时副本，已基于仓库简版补齐规则并同步，skill validator 通过；旧安装版备份在 `.codex-tmp/spec187-two-layer-20260915/installed-skill-before.md`。T001/T003 仍未完成。

2026-09-15 分层纠正：用户确认非 NDNSF＋APP 的依赖应在 base 内构建并验证。暂停完整候选重试，先补齐 base 的运行时与 SDK 闭包，避免 APP 阶段重复构建通用依赖。首轮容器已通过 ONNX/NAC-ABE/SVS/NDNSD 编译，停于 NDNSD metadata 检查；候选未生成，T001/T003 保持 PARTIAL。先前 base PASS 仅覆盖当时 smoke 集合，不代表完整构建依赖闭包；见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-NATIVE-INPUTS：官方 ONNX/Rust 与 Cargo.lock 离线 vendor 已封存，干净源码 handoff `SOURCE_READY`；23 个定向脚本测试通过。旧构建已归档并经内容比较后释放。当前继续构建完整 NDNSF+APP 候选，T001/T003 未完成；这些准备输入由本机生成，不再要求用户提供。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-YOLO-BASE：同一 base 中实际 C++/ORT YOLO26n CPU 推理三次通过（约 182/106/91 ms），50 行输出均满足独立 oracle 容差，最大绝对误差 0.000366211。此结果仅 `YOLO_CPU_MODEL_SMOKE_ONLY`；没有生成最终 NDNSF+APP candidate 或运行 MiniNDN。SDK 已适配，剩余 ONNX/Rust 容器构建输入、host gate 和完整 APP 接线仍由 T001/T003 承接；不把可由现有源码/缓存生成的输入归为用户必须提供的资料。见 [本机 YOLO 记录](evidence/yolo-base-cpu-20260915.md)。

2026-09-15 B187-BASE：r4 STATIC/COMPOSITION_PASS；15 个定向测试通过，实际 SIF 构建与最终镜像 C++ SDK/NumPy/NFD/ELF smoke 全部通过，SHA-256 `7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`。缺失 OpenBLAS 的实际 overlay 反例被拒绝。额外旧门禁测试 6 项 fixture 失败另记，未伪造全套 PASS。仅 base 前置完成，T001/T003 保持原状态；见 [base repair](evidence/b187-base-repair.md)。

2026-09-15 17:51 -05:00：T002 的 C++ selector 已注册并接入 Spec187 native mode；阶段证据现按 request/attempt/plan 关联，并以 epochMs 核对 ACK → Selection commit → Provider accepted → Provider execution 顺序。r7 官方 review-agent 返回 STATIC_PASS；受影响目标以 `-j4` 编译通过（1m6.118s），独立 served-provider selector 通过，缺输入 selector 按预期 fail-closed。已补齐 T001–T006 的显式 FR/SC requirement coverage，分析器追踪到 12/12 FR 与 6/6 SC。真实 through-MiniNDN 请求仍需 candidate-bound config/input，T001 仍缺 regular base SIF/host-gate，T003 及后续批次保持 WAITING_EXTERNAL_INPUT/PARTIAL。

## Logical Batches

| Batch ID | Members | Stable exit | Shared selector / build | Status |
| --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | T001 | closure gate rejects invalid candidate inputs before side effects and accepts a verified pair tuple | existing Tiger script checks and mutation fixtures | PARTIAL |
| B187-BASE | T001 prerequisite | pinned stable base builds and passes native SDK/NumPy smoke | test_base_runtime.py; build-base-sif.py; container C++ base-smoke | DONE |
| B187-APP-SDK | T001 prerequisite | explicit SDK copies load from APP; missing library rejects without fallback | template/handoff/APP tests; container C++ loader probe | DONE (SDK only) |
| B187-YOLO-BASE | T001 prerequisite | real YOLO26n CPU output matches independent oracle inside base | run-yolo-cpu-smoke.py; C++ yolo-cpu-smoke.cpp | DONE (model smoke only) |
| B187-LOCAL-YOLO | T002,T003 | two identical-candidate local C++/MiniNDN terminal YOLO runs | Spec187YoloMiniNdn; host native candidate-bound config/input; SIF/APP execution pending | PARTIAL |
| B187-TIGER | T004 | one bounded same-candidate TigerCluster run | run-sif-app.sh and same selector | WAITING_EXTERNAL_INPUT |
| B187-DEFERRED | T005 | QWEN listed as TODO without entering candidate | docs checks | DONE |
| B187-BASE-SDK | T001 prerequisite | existing base extended with verified external runtime and build SDK | test_dependency_sdk.py; actual container C++/Rust/Python probes | DONE (base only) |
| B187-NDNSF-CONSUMER | T001,T003 | repository targets consume the verified base without rebuilding external dependencies | incremental container build and native YOLO runner | PARTIAL (static and packaging tests passed; actual build pending) |
| B187-CONVERGENCE | T006 | fresh audit PASS before formal local/cluster evidence | CodeGraph plus exact source and symbol checks | PARTIAL |

## Task Details

### T001 Candidate closure and pair mutation gate

**Design binding**: FR-001..FR-003, FR-006, FR-011; existing prepare-development-handoff.py, build-local-sif.sh, build-sif-app.py, validate-sif-app.py and run-sif-app.sh. Preserve their path/digest ownership and add only missing composition or mutation checks.

**Requirement coverage**: FR-001, FR-002, FR-003, FR-004, FR-006, FR-011.

**Success criteria**: SC-001, SC-003, SC-005.

**Outcome**: one command sequence validates source, base, definition, APP, profile and mounts; stale symlinks, host library fallback and changed digests cause zero build/upload/run side effects.

**C++/native acceptance**: N/A for the closure gate itself; native behavior remains T002/T003.

**Risk class / Dynamic profile**: high / none; invariant is zero external side effects on rejected input.

### T002 C++ YOLO selector and MiniNDN caller wiring

**Design binding**: FR-005, FR-009, FR-012; add a registered C++ selector under tests/integration-tests and tests/wscript, and wire Experiments/NDNSF_DI_YoloAckDriven_Minindn.py only as MiniNDN/NFD/identity/process orchestration. The selector must own request/ACK/Selection/Provider/Response assertions and the Face/io_context/scheduler lifetime barrier.

**Requirement coverage**: FR-005, FR-009, FR-012.

**Success criteria**: SC-002, SC-005.

**Outcome**: a named C++ production target invokes the real DI path through the maintained YOLO case; Python-only markers cannot close the task.

Spec187 native mode requires `SPEC187_NATIVE_MODE=1`, the authority inputs
`SPEC187_NATIVE_AUTHORITY_CONFIG`, `SPEC187_NATIVE_GRANT_AUTHORITY_PUBLIC_KEY`
and `SPEC187_NATIVE_PROVIDER_RECIPIENT_KEY_MAP`, plus
`SPEC187_NATIVE_SELECTOR`, `SPEC187_NATIVE_REQUEST_CONFIG`,
`SPEC187_NATIVE_REQUEST_INPUT` and `SPEC187_NATIVE_REQUEST_OUTPUT`. Authority
and request files are candidate-bound under the `results` bind; the sealed
selector is `/opt/ndnsf-di/current/bin/spec187-yolo-minindn`. The runner
validates digests and file identity before `start_network()`, starts the
authority and waits for `NATIVE_GRANT_AUTHORITY_READY`, then launches the C++
selector as the MiniNDN User process. There is no configurable test filter or
post-run DummyClientFace substitute.

**Risk class / Dynamic profile**: high / asan-ubsan; invariant is authenticated selection, terminal result and no active owner after drain.

### T003 Local YOLO pair build and two-run gate

**Design binding**: FR-002..FR-006; reuse the four existing TigerCluster entrypoints and quickstart.md. Do not add a new base builder in this task.

**Requirement coverage**: FR-002, FR-003, FR-004, FR-005, FR-006.

**Success criteria**: SC-001, SC-002, SC-003, SC-005.

**Outcome**: local candidate SIF+APP is built or materialized from a regular base SIF and the C++ selector passes twice with the same pair identity.

**Risk class / Dynamic profile**: high / asan-ubsan; dynamic card freezes nominal, missing/changed path, invalid identity and cleanup cases.

### T004 TigerCluster same-candidate promotion

**Design binding**: FR-007..FR-009; existing run-sif-app.sh and Slurm wrapper only. No rebuild or profile/model substitution.

**Requirement coverage**: FR-007, FR-008, FR-009.

**Success criteria**: SC-004, SC-005.

**Outcome**: one bounded cluster run uses the exact LOCAL_PASS pair; scheduler and facility failures remain separate.

**Risk class / Dynamic profile**: medium / none; invariant is digest/profile equality before request.

### T005 QWEN deferral and delivery record

**Design binding**: FR-010; update only Spec187 scope/checkpoint/evidence references.

**Requirement coverage**: FR-010.

**Success criteria**: SC-006.

**Outcome**: QWEN is TODO and cannot be read by YOLO candidate or acceptance.

**Risk class / Dynamic profile**: none / none; documentation-only.

### T006 Design-code convergence and final evidence

**Design binding**: FR-011, FR-012; inspect actual production call graph, effective configuration, source/build registration, C++ selector and evidence paths with CodeGraph and exact source checks.

**Requirement coverage**: FR-011, FR-012.

**Success criteria**: SC-005.

**Outcome**: severity-classified convergence report returns PASS; any controlling gap creates a repair task before T003/T004 formal validation.

**Risk class / Dynamic profile**: high / none; invariant is requirement-to-production-path-to-evidence agreement.

### T007 Pre-pack candidate closure gate

**Design binding**: FR-001, FR-003, FR-011; the final candidate must contain the
same sealed DI headers, libraries, applications, bindings and manifests that
were built in the container.

**Requirement coverage**: FR-001, FR-003, FR-011.

**Success criteria**: SC-001, SC-003, SC-005.

**Outcome**: assembled runtime tree passes exact source/header closure, SDK
library-origin checks, default-environment native loading, and a real C++
consumer compile/link before any squashfs/SIF packing. A failure preserves the
tree and stops the batch; it cannot be hidden by a later SIF or Python test.

**Risk class / Dynamic profile**: high / none; invariant is one source seal,
one ABI/runtime tree, and one immutable candidate identity.

## Batch Quality Record

| Batch ID | Coverage matrix | Static findings | Compile/build misses | Runtime/test misses | Dynamic validation | Build scope / target / -j / elapsed / exit | Review trace / closure decision | Behavior result | Evidence / remaining |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | production callers, implementation, tests, build, migration: `build-sif-app.py`, `test_sif_app.py`; evidence lane in [b187-local-closure.md](evidence/b187-local-closure.md) | no P0–P3; STATIC_PASS | not run; regular base unavailable | focused offline checks: 3 passed; SIF/runtime not observed | NOT_RUN; dynamic card awaits regular base | not run | review-agent STATIC_PASS on frozen diff; OPEN_FOR_NEXT_BATCH | PARTIAL | regular base SIF and host-gate manifest remain |
| B187-LOCAL-YOLO | production callers, implementation, state/lifecycle, build/source closure, tests/evidence: [b187-local-yolo.md](evidence/b187-local-yolo.md), [recheck](evidence/b187-local-yolo-recheck-20260916.md) | no P0-P3 after segmentation, JSON matcher, ANSI, identity and MiniNDN env compatibility reviews; all returned STATIC_PASS | affected selector compile/link passed with `-j4` 30.668s; native identity manifest regenerated with `SPEC180_NATIVE_IDENTITY_OK`; earlier candidate Waf 293/293 retained | C++ selector, container C++ DI/YOLO unit, native runner and three host through-MiniNDN Y-A runs passed; segmented 6,555,271-byte input observed as 1,601 segments; maintained r39 run passed after parser compatibility fix | host MiniNDN observed; SIF/APP qualification unobserved | host build `-j4` 30.668s; r27 40.17s; r28 39.62s; r39 41.90s; candidate SIF `e6cef05a…541657` not executed in this batch | review-agent STATIC_PASS; host terminal chain observed; OPEN_FOR_NEXT_BATCH for SIF/APP closure | PASS (host MiniNDN scope) | T001 host-gate/pair mutation, same-pair SIF execution and T004 Tiger promotion remain |
| B187-TIGER | no execution because T003 has no LOCAL_PASS; [b187-tiger-yolo.md](evidence/b187-tiger-yolo.md) | N/A before local gate | not run | not run | NOT_RUN | not run | review-agent N/A; BLOCKED_BY_LOCAL_GATE | WAITING_EXTERNAL_INPUT | T003 LOCAL_PASS and external TigerCluster access remain |
| B187-DEFERRED | documentation lane covered; other lanes N/A by scope | N/A by docs-only scope | N/A | N/A | N/A | N/A | review-agent N/A; CLOSED_FOR_VALIDATION | DONE | QWEN remains TODO; [qwen-deferred.md](evidence/qwen-deferred.md) |
| B187-CONVERGENCE | production/callers, implementation, state/lifecycle, build/source closure, evidence: [convergence-20260915-r1.md](evidence/convergence-20260915-r1.md) | no P0-P2 after r7 review | target compile/link passed | missing-input selector fail-closed; real MiniNDN/SIF not observed | NOT_RUN for formal qualification | build boundary recorded in B187-LOCAL-YOLO | review-agent r7 STATIC_PASS; CLOSED_FOR_VALIDATION | DONE (static) | external candidate inputs and formal runs remain |
| B187-PREPACK-CLOSURE | source/header/library/app closure, actual C++ consumer and evidence: [b187-complete-candidate.md](evidence/b187-complete-candidate.md) | T007 frozen closure review STATIC_PASS; HOME isolation reviews found and closed P1 | candidate builder Waf 293/293, binding wheels and final native verifier passed; documented pre-pack C++ consumer remains unexecuted | SIF-inside `unit-r2` C++ consumer passed, but it is post-pack and does not satisfy the pre-pack ordering claim | NOT_RUN for pre-pack consumer | candidate SIF `e6cef05a…541657`, Apptainer 1.5.3, `BUILT_UNQUALIFIED` | T007 static PASS; OPEN_FOR_NEXT_BATCH until pre-pack consumer gate is made true | PARTIAL | either run consumer before packing or correct plan/acceptance ordering; keep unit-r1/yolo-r1 failures |
| B187-CONTAINER-UNIT | candidate container C++ compile/runtime, source/SIF identity, test fixture, evidence: `.codex-tmp/spec187-clean-restart/unit-r2/` | smoke-driver HOME fix STATIC_PASS; no P0-P3 | tests compiled inside candidate with candidate pkg-config and libraries | 4 C++ DI/YOLO native test sources passed; `DI_CPP_UNIT_SMOKE_PASS`; unit-r1 HOME failure retained | SIF candidate observed; MiniNDN not exercised | container compile/run 52.56s, rc=0, SIF `e6cef05a…541657` | review-agent STATIC_PASS on driver repair; CLOSED_FOR_UNIT_SCOPE | PASS (unit scope) | not request-chain or MiniNDN qualification |
| B187-YOLO-NATIVE | candidate SIF, ORT/C++ runner, model/fixture/oracle hashes, evidence: `.codex-tmp/spec187-clean-restart/yolo-r2/` | smoke-driver HOME fix STATIC_PASS; no P0-P3 | candidate SIF compiled native runner against candidate DI/ORT libraries | three native runner inferences passed; 50 rows each, max abs error `0.000534058`; `YOLO_CPU_NATIVE_RUNNER_PASS`; yolo-r1 HOME failure retained | local C++ model smoke only | container compile/run 9.76s, rc=0, SIF `e6cef05a…541657` | review-agent STATIC_PASS on driver repair; CLOSED_FOR_YOLO_SCOPE | PASS (model/runner scope) | no authenticated NDNSF request chain or MiniNDN qualification |

### Batch Retrospective

- static: T001 and T002 review-agent gates are STATIC_PASS; r3/r4 found the output-collision and marker-correlation issues, r6/r7 confirmed their repairs.
- compile/link: candidate container Waf compiled/linked `293/293` targets with `-j4` (16m4.970s), including native provider executables and both bindings; the first source/header and test-driver compile misses remain recorded.
- runtime/test: candidate SIF C++ unit smoke `unit-r2` passed four DI/YOLO native test sources and candidate native YOLO `yolo-r2` passed three ORT/tensor-codec runs; `unit-r1`/`yolo-r1` HOME failures remain as raw boundaries. Host MiniNDN Y-A positive terminal runs r27/r28/r39 passed with the segmented request and the maintained runner; MiniNDN negative paths remain unobserved.
- unobserved: host-gate/pair mutation, same-pair SIF execution, MiniNDN negative paths, and cluster run. The pre-pack external test-consumer gate described in the plan was not separately executed; unit-r2 is post-pack evidence. Base smoke and APP SDK loader checks are recorded separately in [B187-APP-SDK](evidence/b187-app-sdk.md); they do not close the request chain.

2026-09-17 B187-C++-SEGMENTED-WORKER-REGRESSION（FOCUSED_PASS）：修复
`tests/wscript` 中 `unit-tests` 漏列 `OperationRuntime.cpp` 的链接闭合后，复用
`build-spec187-local-nac-r1` 完成 unit/worker 夹具增量构建。C++
`Spec182OnnxWorkerProtocol/*` `30/30`、
`GenericDynamicApi/PreparedAndMessages/LargeDataPublicationEmitsBoundedFinalizedSegments`
`1/1`、`Spec175NativeAssembly/*` `7/7` 以及
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput` `1/1` 通过。
本批只证明 worker frame 上界、FinalBlock 分段、SegmentFetcher 重组和 assembly 回归；
不改变 T001/T003/T007 的 `PARTIAL`，不计为 SIF/APP 或 Tiger qualification。完整日志、
失败边界和磁盘记录见 [Qwen replay evidence](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--segmented-worker-and-large-data-regression-closure)。

## Dependencies & Execution Order

T001 → T007 → T002 → T006 → T003 → T004. T005 is independent documentation work and must not add a dependency to the YOLO path. T007 remains a mandatory pre-pack closure gate in the design; this candidate proves the builder verifier and post-pack C++ consumer, but the separately documented pre-pack test-consumer step was not executed and therefore remains open. Each code task receives its own static review before batch composition review, and each later gate consumes the exact immutable identity from the previous gate.

2026-09-16 segmented-input selector current recheck：复跑
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput` 返回
`rc=0`，1 case、6,449 assertions 全部通过；6,555,271-byte 输入的 4,096-byte
分段和 Provider `SegmentFetcher` 重组边界仍通过。该验证不改变 T001/T003 的
`PARTIAL`、T004 的 `WAITING_EXTERNAL_INPUT`，也不把 Qwen 或 SIF 计为 PASS。
原始输出为 `.codex-tmp/spec187-segmented-input-focused-current-20260916.log`。

2026-09-16 B187-LOCAL-YOLO repeated current-source replay：同一 candidate-bound
配置/输入、匹配 NDN-SVS 依赖和新的 root-owned state/output 根再次完成 Y-A。
`SPEC180_CASE_RESULT status=PASS`、`PASS/TERMINAL_RESPONSE/TERMINAL_RESPONSE_VERIFIED`、
`SPEC187_NATIVE_REQUEST_PASS` 和 7,267-byte native result 均已记录，1,601 个输入
分段与子进程清理再次通过。普通用户目录导致的 `STATE_ROOT_OWNER_MISMATCH` 仅为
协议前置失败，原始日志已保留；详见 [repeated replay](evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-repeated-current-source-local-yolo-replay)。
本轮仍未执行 SIF/APP host-gate、同 pair SIF、negative path 或 Tiger，故 T001/T003/T007
保持 `PARTIAL`，T004 保持 `WAITING_EXTERNAL_INPUT`。

2026-09-16 C++ large-data publisher regression：本轮先修复测试夹具误触发 NAC
公共参数同步等待的问题，新增只绑定签名 KeyChain 的 LocalMock test hook，并将
`DummyClientFace::processEvents` 改为负 timeout。官方只读 review-agent 对三文件
范围返回 `STATIC_PASS`；临时 `spec187-large-data-publisher` selector 以 `-j4`
构建成功（1m46.201s），单用例 `28/28` assertions 通过。20,000-byte synthetic
明文被验证为连续 finalized segments，签名、最大 7,000-byte content、最大
8,800-byte wire、envelope/AAD/AES-GCM 重组均通过；原始日志见
[large-data publisher evidence](evidence/b187-large-data-publisher-cpp-20260916.md)。
`tests/wscript` 已恢复。该 focused C++ 结果不改变 Qwen、SIF/APP、MiniNDN
negative path 或 Tiger qualification 状态；T001/T002/T003/T007 仍为 `PARTIAL`，
T004 仍为 `WAITING_EXTERNAL_INPUT`。
同一当前共享库复用的 framework-closure 动态回归也通过（26/26 cases、204/204
assertions），记录在上述证据文件中。

2026-09-17 **B187-LARGE-FETCH-OWNERSHIP / FOCUSED_PASS**：大对象 Provider fetch
路径完成 shared `ConstBufferPtr`/shared envelope 修正；静态复审无 P0/P1/P2，受影响
构建 `233/233`、`-j4`、峰值 RSS 2,868,132 KB、无 swap。`Spec175NativeAssembly/*`
`7/7` 和分段 request selector `1/1` 通过。该批未启动真实 Qwen、SIF 或 Tiger，
不改变 T001/T003/T007 的 `PARTIAL` 状态；完整边界见
[large-fetch evidence](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--large-fetch-ownership-repair)。

2026-09-17 **B187-QWEN-R14 / RESOURCE_BOUNDARY**：共享 WireEncode buffer 修复后的
新 Qwen 本机 run 在正确环境下启动 Controller、Authority、三个 Provider 和 requester，
Provider 均发出签名 V3 offer；约 117 秒时资源监控因 `MemAvailable=1,651,808 KB`
（进程树 RSS `6,737,232 KB`）触发 2 GiB 安全线，wrapper 返回 `rc=137`。未观察
Selection、Provider execution、terminal response 或 numerical oracle；r14 仍是
`RESOURCE_BOUNDARY`，不改变既有 YOLO host PASS、SIF/Tiger 状态，T001/T003/T007
继续 `PARTIAL`。所有 r14 子进程已核验退出，磁盘约 43 GiB 可用；模型、当前 build
和 1.5 GiB run directory 保留。详见 [r14 monitored replay](../184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md#2026-09-17--r14-monitored-replay-after-shared-wireencode-repair)。
下一批必须先改变大对象发布的存储边界（有界或 file-backed segment serving）并通过 C++
资源探针；不能只提高阈值或删除模型、build、run directory 和原始证据。

2026-09-17 **PROPOSAL-CRITERIA-REVIEW / DOCUMENT_PASS**：按公开 proposal 标准
完成有限文档修订和必要性复审；双语正文、slides、PPTX、notes及两种对照稿同步。
英文68页、中文52页、slides共50页（42页主线）；双语镜像/入口一致，LibreOffice
回转文字一致、PPTX文字可编辑、对照工具19项测试通过。修改不涉及实验数据、
产品实现或资格状态，T001/T003/T007保持 `PARTIAL`、T004保持 `WAITING_EXTERNAL_INPUT`。
证据：[criteria review](../../docs/PAPER/proposal-defense/proposal-criteria-review-20260917.md)
及[validation](../../docs/PAPER/proposal-defense/proposal-criteria-validation-20260917.json)。
原始构建/备份在 `/tmp/ndnsf-proposal-criteria-20260917-cGu4YP/`；首轮验证器漏计
titlepage已记录并修正，最终验证通过。仍需补全文献机制级对照与冻结实验配置，
不将文字审查当科学结论。既有全index checkpoint hook阻塞仍未绕过；本条及
failure-log与并行进度共存，不整体暂存它们。

同轮 checkpoint 只读预检返回1，首个命中仍为
`.specify/memory/constitution.md:16`；未改变 index、未生成新commit，HEAD仍为
`d5b241e6`。文档状态 `DOCUMENT_PASS` 与提交状态 `BLOCKED` 分开记录。
原始预检见本轮 run 的 `checkpoint-preflight.log`；最终交付文件摘要与已验收产物一致。

2026-09-17 **PROPOSAL-CONTRIBUTION-REVISION / PARTIAL**：三项研究修订已完成
双语正文及52页slides/PPTX构建，artifact检查通过；产品与实验任务状态不变。
inline对照首次在上游报告生成前启动而失败，保留
`/tmp/ndnsf-contribution-revision-20260917-oPAr8a/inline-build.log`；paired现已完成，
须顺序重建inline、核验并归档后才关闭本文档单元。参见
[failure boundary](../../docs/failure-log.md#2026-09-17--proposal-contribution-comparison-dependency-boundary)。

同轮inline对照已通过；slides第二轮收束语造成0.90697pt布局回归，文档单元仍
`PARTIAL`，须重新构建并覆盖交付。持久边界见
[layout regression](../../docs/failure-log.md#2026-09-17--contribution-slide-wordinglayout-regression)。

2026-09-17 **PROPOSAL-CONTRIBUTION-REVISION / DOCUMENT_PASS**：完成上述三项修订，
英文72页、中文55页、52页slides/PPTX（44页主线）及notes同步，最终验证 `PASS`。
两类镜像和slides入口一致，PPTX回转逐页文字一致、文字可编辑；最终无Overfull、
缺字或未定义引用。原实验frames与abstract未改；19项对照测试通过，101页金色
对照及131页左右对照覆盖349个旧单元、334个新单元。旧对照资源可从run目录恢复。
文献差异证明和实测取舍仍待完成，详见
[revision](../../docs/PAPER/proposal-defense/contribution-revision-20260917.md)及
[validation](../../docs/PAPER/proposal-defense/contribution-validation-20260917.json)。
未运行产品／SIF／Tiger实验；T001/T003/T007仍 `PARTIAL`，T004仍
`WAITING_EXTERNAL_INPUT`。全index hook阻塞未绕过，HEAD仍 `d5b241e6`，本轮修改
未提交；进度和failure-log与并行工作共存，不整体暂存。

2026-09-17 **RQ1-COST-MODEL / PARTIAL**：中英文成本模型与G1/G2/G3计划配置已写入，
53页slides/PPTX初轮文字一致性通过，视觉间距修订后等待最终复核。对照PDF构建
触发新版字符覆盖断言，须修复提取边界再关闭文档单元。证据见
[failure boundary](../../docs/failure-log.md#2026-09-17--rq1-cost-model-comparison-extraction-boundary)。
本轮无产品实验；T001/T003/T007与T004状态不变。

2026-09-17 **RQ1-COST-MODEL / DOCUMENT_PASS**：成本模型、G1/G2相同权限矩阵、
G3授予／撤销压力测试已写入双语proposal与slides。英文73页、中文56页、53页
slides/PPTX及11页notes同步，最终artifact检查通过；截图修复第20页表格间距。
对照工具数学字体提取边界已由先失败后通过的真实PDF测试锁定，20项测试通过；
103页inline和134页左右对照完整覆盖349旧单元、343新单元。见
[review](../../docs/PAPER/proposal-defense/cost-model-review-20260917.md)及
[validation](../../docs/PAPER/proposal-defense/cost-model-validation-20260917.json)。
本轮没有运行产品实验，所有新增数量为分析示例／计划配置，原实验结果未改。
T001/T003/T007仍 `PARTIAL`，T004仍 `WAITING_EXTERNAL_INPUT`。全index提交预检
仍 `BLOCKED`，未绕过hook或改用户暂存区；本轮修改未提交。
