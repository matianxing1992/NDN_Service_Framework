# Spec190 Audit

## Strict Serial Revision — 2026-09-22 03:05 -05:00

用户要求：清单即执行顺序，上一项全部本项验收完成才进入下一项，不允许前向依赖或批内欠测推进。
以`832cc8b7`为基线重排未执行任务，映射见tasks checkpoint；旧B190/证据身份保留。
当前ID：T006传输、T007固定Repo、T008 prepare复用、T009保护材料、T010收敛、T011系统实验。
下文旧版审计中的T ID均为历史编号，不作为当前调度依据。

**Findings / repair**：修正原收敛/实验排在追加前置之前；正文/registry/plan统一单链DONE依赖。
Kant指出T010 Read包含后项、plan仍可并行及T004/T006本项/系统验收混淆，均修正并局部复审PASS。
T004本项原生控制/生命周期完整验收，T006本项真实序列化/计数校准完整验收；
真实MiniNDN≤2秒及保护材料热路径完整归对应T009/T011，原SC不减少、不把欠测改名为完成。
共享skill增加按用户显式要求启用的STRICT_SERIAL，默认模式不变；本机tasks入口和版本化模板引用同一规则，个人共享副本同步。
Laplace检查器审查发现子ID截断与模式声明宽松，改完整ID解析/显式声明并加反例；单层检查器明确拒绝可执行子ID。
Laplace对该增量复审PASS；main实际运行正常链+10反例PASS，不以代理未运行的静态意见代替测试结果。

**Coverage matrix**：production entry为本机speckit-tasks入口及版本化模板；implementation为task-progress严格模式与顺序检查器；
test为正常链和10个非法结构fixture及真实tasks解析；build/source closure为Python工作流工具、native N/A；
migration/evidence为旧→新映射、保留历史证据及局部DONE/系统PASS边界。无产品源码/API变化。
**Validation**：strict结构17 FR/9 SC/11 tasks/0完成；顺序检查及自测、skill格式、sync11/11+personal、diff检查。
**Review trace**：profile沿用下述review-agent路径/SHA；Kant审Spec顺序/验收，Laplace审skill/检查器，均只读。
初次冻结`.codex-tmp/spec190-serial-review/review-v1.tgz` SHA256 `7b78a2b7d124039dd3c0cb89a29454f795411e79f66e53d305989f685f78269b`；
后续只对上述已定位问题修复并复审。Batch growth decision：仅顺序/验收归属/skill门修订，不扩产品任务。
**Retrospective**：static发现/修复如上；compile-link N/A；workflow-runtime为检查器自测，native-runtime NOT_RUN；
unobserved为全部产品任务与真实性能。Closure decision: CLOSED_FOR_VALIDATION（文档/工作流），不表示产品完成。
**Checkpoint boundary**：仅Spec190与干净的任务模板/进度契约/新检查器；既有21个暂存文件不混提。
本机`.agents/skills/speckit-tasks/SKILL.md`受忽略，已更新但不强制入Git；个人共享副本仅本机同步。
Design变更索引仍含既有混合修改，保留未提交。下一步仅T001；T009生产编码仍有安全设计门。

## Verdict

PASS — 限定本次文档规划：新增Repo/传输/prepare复用范围完成主审、结构检查及Kant独立复审修正。
结构PASS：17 FR、9 SC、5 stories、11 tasks、0产品勾选、17/17 FR追踪。
不是全Spec READY_FOR_IMPLEMENTATION；T004先诊断FINALIZE首边界，T011先冻结CD-09安全接口。
产品实现/行为/性能NOT_RUN。以下原7任务审查保留为历史，不代表扩展范围已获独立PASS。

## Scope Revision

相对checkpoint `930f69e9`增加CD-06–09/T008–T011：实际stage tensor预算与分层缓存计数，
固定每node Repo owner和重启恢复，prepare前lookup免重复拆层/导出/打包/STORE，当前授权下合法复用。
已修正旧版“Repo全部不在范围”、7任务追踪和仅STORE去重的歧义。
源码核对表见research；已有file backend/manifest-first基础不等于真实protected重启复用。
CodeGraph有待索引文件，结论以直接源码核对为准。校验读盘不等于网络payload，layer hit不等于resident hit。
五lane按plan B190-08–11；static主审完成，compile-link/runtime-test NOT_RUN，
unobserved为实际prepare跳过量、wire预算、重启后合法密文服务及磁盘稳定性。

### Revision Review Trace

review profile沿用下述路径/摘要。两reviewer首次遇model capacity，保留状态后重试；不视为产品失败。
Kant针对冻结快照发现T010只恢复publication receipt不足以恢复PreparedModel；已补ModelPreparationCache前置分支、
PreparedMetadataV1、reference-only catalog/adapter恢复、当前Runtime owner重建及真实冷正/热零计数，局部复审PASS。
v1 `.codex-tmp/spec190-reuse-review/doc-snapshot.tgz` SHA256 `a9bdad06aa9eabdbddcf13a6042281e34889c202f24b38c5535011bfb3372bb9`；
修正后v2 `.codex-tmp/spec190-reuse-review/doc-snapshot-v2.tgz` SHA256 `44c276bc9d29f9c2449a2b94edf0ab3efff9b9bc64e54b73fe1b3dded9fdbd09`。
结构检查PASS，Spec Kit入口同步11/11，diff whitespace检查PASS；未执行产品构建或实验。
Laplace完成冻结快照的传输/缓存范围复审，无新增控制性发现；T010最后增量由Kant复审，不冒称两人均独立重验。
Closure: CLOSED_FOR_VALIDATION（规划交付），T004/T011前置门保留，所有产品任务仍未完成。

## Scope and Source

用户要求多轮token生成延迟改善、1秒ACK候选、详细任务、排除非核心内容。
源码在Experimental脏工作树，HEAD b9c6930b46e2bf9d91c5ae5732403bb5728db003；
历史r260 raw保留。真实行为/样本口径见research；文档不能冒充新运行结果。

## Findings and Repairs

| Severity | Finding | Repair / detection |
| --- | --- | --- |
| HIGH | 将30秒归成stop固定等待没有证据 | research/CD-03/T004改为FINALIZE控制闭环，先定位首边界，保留补偿语义 |
| HIGH | 旧profile若重绑定会冒充当前请求 | CD-04分离load provenance与新request observation，C++污染反例；CUDA资格不放宽 |
| MEDIUM | checkpoint issuedAt及Provider执行事件容易被称TTFT | research标清来源；T001/T007新口径成对重测，不混合历史数字 |
| MEDIUM | 只缩ACK遗漏CLI缓冲、同handle及轮间等待 | T003实时消费/持久对象，T004终态闭环，SC逐阶段独立报告 |
| MEDIUM | ACK early-close增加复杂度但收益最多约1秒 | 本Spec剔除，先固定1000ms；不改通用服务默认或新增自适应算法 |
| MEDIUM | 虚构RequestHandle.cpp路径 | 改为现有PreparedModel.hpp/.cpp，核对Conversation实际const签名 |
| MEDIUM | EventReader内存fixture不能证明CLI即时flush | T003/CD-02加真实CLI子进程pipe、暂停后续生成、暂时空队列和失败退出反例 |
| MEDIUM | FINALIZE缺部分提交反例 | T004/CD-03加一侧COMMIT/另一侧ACK丢失，不提交成功checkpoint并补偿 |
| MEDIUM | single-flight发起者取消owner不明确 | CD-04明确cache-owned load与请求waiter分离，T005加首loader取消/全部取消/close后迟到完成 |
| MEDIUM | decode主指标含义不清 | SC-006固定requester接收间隔、首token/EOS/finalize排除和逐轮配对统计，ORT耗时单列 |
| MEDIUM | 默认一槽范围及explicit evict/close准入不够明确 | 仅Spec190 CPU profile显式启用；补evict(key)及Retiring/closed拒绝新租用、drain语义与并发反例 |
| MEDIUM | ASan不能覆盖shared cache的data race | T005加限定生产owner的TSan ResidentSessionConcurrency；不扩成第三方ORT整体race资格 |

## Coverage and Evidence Limits

五lane规划覆盖见plan；未实现测试/Waf注册明确planned，任务均未勾选。
只读CodeGraph+源核对、r260日志派生分析；本轮无产品修改、编译、安装、MiniNDN或清理模型。
Dynamic profile/build/runtime均NOT_RUN（纯文档工作）。T004生产修复设计门仍需真实首边界，不能全Spec标READY_FOR_IMPLEMENTATION。

## Review Trace

main agent完成source-reality和任务双向追踪；Laplace在冻结v1关闭其四项发现和handle术语问题；
Kant在冻结v2确认启用范围/evict/close问题关闭，仅要求加限定TSan gate，已在plan/T005补齐。
v1 `.codex-tmp/spec190-planning/review-v1.tgz` SHA `2f53e6b0a53c89de49831a40bd944349b0c22b530694892a1ee4d7555ae70530`；
v2 `.codex-tmp/spec190-planning/review-v2.tgz` SHA `b9d26d3fa32bd30259ee14434a96d1cfb261c7c2378b43f765c73bf50e6032de`。
两agent均只读，无委托产品修改；Kant追加核对最后TSan两处delta并返回planning PASS，所有所提发现关闭。
profile `/home/tianxing/.codex/skills/review-agent/SKILL.md`，
SHA `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
无DeepSeek调用。Batch growth decision：只保留7个核心行为/验收任务，不拆行政子项。
结构gate：12 FR、6 SC、3 stories、7 tasks、0产品勾选、12/12 FR映射；10份文档相对文件链接与7行进度映射通过。
Spec Kit sync PASS 11/11；feature pointer与本机managed plan指针已切190，Context Mode索引刷新后project/active health通过。
Closure decision: CLOSED_FOR_VALIDATION，仅表示本次规划文档交付闭合；产品实现/性能验证未开始。
Checkpoint scope：仅新Spec190十份文档和`.specify/feature.json`；旧Spec189交接及Design记录含既有混合改动，
保留工作树不混提。本机AGENTS managed pointer在忽略文件内，仅本机更新。使用hook既有
`NDNSF_LOCAL_CHECKPOINT=1`文档checkpoint入口，保留禁止路径检查，不改hook、不使用no-verify、不push。

## Batch Retrospective

static：如上术语、范围和FINALIZE归因已修；compile-link：NOT_RUN；runtime-test：NOT_RUN；
unobserved：1秒真实成功率、真实TTFT/吞吐、FINALIZE首个缺失点、CPU驻留节省量及退出资源。
本轮不补造构建耗时，不用任务数量作性能证据。
