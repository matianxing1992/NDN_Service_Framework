# Spec190 Audit

## Verdict

PASS — 限定文档规划范围：结构检查、独立只读复审及所提问题修订完成。
不是全Spec READY_FOR_IMPLEMENTATION；T004仍先诊断FINALIZE首边界。产品实现/行为/性能NOT_RUN。

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
