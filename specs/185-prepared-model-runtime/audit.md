# Proposal Audit

**Date**: 2026-09-12 | **Verdict**: CONDITIONAL PASS for revised design; implementation NOT_STARTED.
**Source baseline**: `575b43cc93bbed29932303caf3d09974f1585af7`。源码行号以本次读取为准。
原草案为用户附件；本记录保留问题与修订，不能把草案中的行为视为已实现。

## Findings and Resolutions

| ID / Severity | Original issue / source evidence | Revised decision / owner |
| --- | --- | --- |
| A01 HIGH | 将 inspectModel 调用理解为每次重解析不准确：NativeCanonicalPreparationCatalog.cpp:54–77 构造已验证；115–118 回调查冻结记录；NativeRequestPreparation.cpp:194–214 保留请求绑定 | prepare 补 façade/get-or-create/lease；保留轻量输入/model 检查。C-02/T003,T005 |
| A02 HIGH | NativeRequestCatalog.hpp:5–6 明确不认证远程 metadata；hash 不是来源认证 | 本期 operator-pinned catalog + 既有可信 Data fetch；不扩展远程目录协议。C-02/T003 |
| A03 HIGH | 原 CachePolicy 文字与 miss 主动 fetch 冲突，未定义 waiter/Refresh 原子性 | 默认 UseOrFetch；四态策略表、独立 deadline、generation CAS、budget/lease。C-02/T004 |
| A04 HIGH | model cache 容易冻结 grant/epoch/placement；NativeRequestPlanner.cpp:315–355,450–474 为 request-bound 路径 | cache 不复用执行权限；ACK后动态 placement/grant，热缓存撤销反例。C-03/T005,T006 |
| A05 HIGH | manifest 中统一 roles/candidates 不适合不同切分与请求预算 | 冻结 splitter/catalog，候选和角色在本请求预算下验证。C-03/T005 |
| A06 HIGH | Provider 缓存被草案当已有；DI_NativeProviderExecutable.cpp:1735–1778 每次 post-Selection prepare，NativeProviderHandler.cpp:2226–2242 创建 runner | 作为独立新增批次；分 artifact/template/mutable runner；保留每次 guard。C-03/T009,T010 |
| A07 HIGH | prewarm bool 与认证后 fetch 冲突；NativeCanonicalOnnxAssembler.cpp:452–481 明文 lease 受保护 | 去除公开 prewarm；不得共享 mutable KV/旧 grant/plaintext。C-03/T010 |
| A08 HIGH | Runtime owns IO 但阻塞 prepare/wait 未定义 deadlock/close/最后 owner | owner线程拒绝阻塞；close与drain区分；析构与强引用环反例。C-01/T002 |
| A09 MEDIUM | 全局 ASSEMBLING/RUNNING 会伪造分布式观测；Provider executable:1779–1785 不采用全局 ReadySet barrier | 保留 NativeRequestStatus，内部 role progress 不新建全局 barrier。C-01/T006 |
| A10 MEDIUM | source 两处、placement 两入口、wait无参数未声明、abstract factory按值、错误 API 已部分存在 | source唯一归属、单placement覆盖、wait双重载、复用内部factory/NativeDiError。C-01,C-03 |
| A11 HIGH | Spec184 尚有资格/外部模型/retirement 边界，不能因185创建清零 | 184保留原任务；185接入基线定向验证、独立新回归 owner。plan.md |

## Code Reality and Limits

已按 CodeGraph-first 查询并用真实维护源码复核；宽查询混入 `.codex-tmp` 历史副本，
最终结论只采用上表 canonical 路径。共享目录前缀为 `NDNSF-DistributedInference/cpp/ndnsf-di/`。
没有运行模型、native build 或实验，不能从本审计宣称性能改善；当前 canonical 路径本来就有冻结目录。
最主要收益是应用 API 简化、准备生命周期统一和可验证复用，而不是未经测量的推理加速。

## Remaining Gate

交叉文档复审额外修复：A12配置profile假设（C-01单用户default、C-03复用真实Provider argv）；
A13完整taskContract/tokenizer/composition/inputLayout身份加入Package/key；
A14 Provider独立预算/single-flight/原子发布；A15批次验收依赖环（B6–B9拆成单任务出口）。
这些修订不增加任务数，避免接手者在实现中重新发明关键契约。

范围设计已修订；T001/T002 实现须以当前 native config/IO owner 为基线。
本轮不声称源码 STATIC_PASS 或184资格完成。所有 planned suite 和新 façade 均需实际实现与注册。
每个批次的未执行测试、动态卡和构建身份记录于 [validation](contracts/validation.md)。

## Review Trace

主线程执行 speckit-audit/codegraph-first/source verification；独立只读研究代理
`audit_prepared_model` 审查 canonical catalog、planner、Provider assembly 与 cache/security 边界，
返回8项发现，均已纳入 A01–A10。该研究复核不是实现后的官方 review-agent STATIC_PASS。
本轮 source diff 为零；文档检查结果集中在 [planning evidence](evidence/planning-20260912.md)。

## API-wide Revision

[API审计](api-review.md)覆盖76个项目C++头、139个Python模块及binding/install候选；20项发现见C-05，独立C++与先原生后包装见C-06。原草案审计保留历史边界。
新增T015安装/ABI、T016扩展freeze/control，T013移到T012之前，16任务11批全部NOT_STARTED。
