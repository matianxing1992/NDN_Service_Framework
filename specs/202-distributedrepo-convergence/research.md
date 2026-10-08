# Research and Design Decisions

本文件是仓库静态分析与设计决策，未开展文献/性能比较，不生成research performance结论。

| ID | Decision | Rationale / alternative |
| --- | --- | --- |
| D1 | 保留现有native双mode及各store/Core owner | 主路径已存在；重新建Repo或Python catalog无法解决三个实际缺陷，反而扩大迁移面 |
| D2 | destination旧namespace保留到post-sync deadline接受 | 仅移deadline或吞fsync违背现有契约；same-parent hardlink/恢复局部化，不引入通用transaction框架；不支持link时覆盖前失败 |
| D3 | ownerSourceFailed按attemptCount区分unknown与never attempted | 原finalizer已有该区分；将已发请求变SKIPPED会丢失可能latecommit事实，局部修正优于新state protocol |
| D4 | verified legacy destination replay补parent barrier | 保留完整目标与source/no-replace；强制删除已经暴露目标不符合existing observer contract |
| D5 | A19/O07/A20按不同plane承担fault oracle | 原意要求区别缺内容、缺段与offline，没有要求artifact API接收传统parent；不新增legacy availability API |
| D6 | A21解析签名Sync映射，不加observer | 已有抓包有真实publication sequence；targetProvider没有编码到该Request，observer无法制造字段；直接观测与ordinal推断分别标记 |
| D7 | metadata授权与原始Data producer encryption分离 | 原名Interest无NDNSF grant；hint只路由。不能把元数据拒绝或hint当内容保密/serving-source crypto证明 |
| D8 | final candidate按共同source+显式profile composition | object/Persistent/import/6/7host profiles不能互换lock；历史localPASS不能合成current整体资格 |
| D9 | 文档成形与产品readiness分别判断 | 本次仅static+planning；已知HIGH仍BLOCK product，任务未实施；不因结构scan或CodeGraphhealthy称产品PASS |

## Resolved Environment Facts

system Boost1.71与pinned Experimental SVS；build≤j4且不并发同一tree；unprivileged build/tests，sudo-n仅MiniNDN和精确目录操作。现有Repo library是static，focused API selectors可用test-only`--wrap=fsync`，需要assert实际hit和避开准备/回滚sync。

## Remaining Design Checks

source reconciliation/public entry inventory、legacy permissive parser适用compat边界、同destination并发/特殊filetype支持与actual rollback failure分类必须在T001/T002编码前核对。它们有owner与closing gate，不以不确定事实宣称实现。GSD采用activeSpec registry；无额外manager；ARS统计与Tiger/SIF不属于本Spec本机功能目标。
