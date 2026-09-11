# Audit-Driven Execution Plan

**Revision**: 1 | **Date**: 2026-09-11 | **Status**: ACTIVE; implementation status maintained in tasks.md
**Authority**: 用户要求新的安排以本次审计为准；[审计报告](../evidence/request-chain-static-audit-20260911.md)。

## Precedence and Preservation

本文件与 tasks.md 的 R12 registry 是剩余工作的唯一调度入口，取代旧 R4–R11 的 next-dispatch
文字和未执行任务卡。旧计划、勾选和证据保留历史，不删除已通过的事实，也不把被替代任务算作完成。
T001–T017、FR/SC/PO 是验收要求，不因执行重排失效。N1–N5 的独立 authority、C++ 优先和
实验机分工仍有效；旧阶段中已经存在的生产路径不重新实现，已验证出口按源码影响复用。
任何尚未归入新表的旧 OPEN 项须先按本次审计归类，再补入对应批次，不能继续隐式派发。

## Current Dispatch Registry

状态唯一维护于 [tasks R12 registry](../tasks.md#r12-active-task-progress-registry)，本表定义依赖和退出契约。

| Batch | Depends | Parent obligations | Scope and observable exit |
| --- | --- | --- | --- |
| R12-A Thread ownership and cancellation | 审计 F-01/F-02 | T005/T010/T011 | authority submit 仅在 Core IO owner 上发生；Operation turn/attempt 快照与发布同步；cancel/deadline/close 与规划交错时唯一终态且无残留 ticket |
| R12-B Durable commit outcome | R12-A | T010/T011 | F-03：durable publish 与成功 outcome 同一线性化决定；发布前取消不提交，发布后取消不报普通失败；FINALIZE 失败不回滚已提交 parent |
| R12-C Checkpoint export | R12-B；技术上可独立，但默认顺序执行 | T010/T011 | F-04：文件从首次可见即0600；完整写入与原子替换；失败保留旧 checkpoint；导出可再加载 |
| R12-D Caller and mode convergence | R12-A/B/C 的定向 C++ 出口 | T013/T015 | G-01：重建 caller/mode 清单；将目标生产默认路由接入同一 C++ owner；每个遗留入口标为 migrated/explicit compatibility/removed，并有可追溯 oracle |
| R12-E Native qualification matrix | R12-D；按缺项执行，不全量重启 | T014/T016/T017 | G-02：当前源码下 C++ unit/integration/process、I/PO、no-Python 与依赖闭包逐项有证据；剩余外部资格保持明确状态，不能以本机 tiny 通过关闭 |

## Atomic Tasks and Batch Gates

R12-A 分为 A1 authority IO dispatch/late completion、A2 Operation mutable-state/turn publication。
R12-B 分为 B1 提交线性化及 cleanup 顺序、B2 client-level 确定性交错 fixture。
R12-C 分为 C1 安全原子导出、C2 round-trip 与失败保留 fixture。
R12-D 首先 D1 生成当前 caller/mode 清单，再按共享契约和后端归为 D2.x coherent groups；
不得把旧“16 调用点”机械展开为16轮构建。R12-E 先 E1 对账矩阵和失败首边界，再按同一
生产 target/fixture 组成 E2.x 验收组；不凭 Python 测试数量推进 native 状态。

每个小任务完成代码后明确加载独立官方 review-agent，记录 skill 路径/hash、基线、完整 diff、
调用方和测试审查、finding 与复审结果；只读阶段不编译、不修代码。每个逻辑批次的所有小任务
静态门通过后做组合审查，再统一构建与 C++ 单测/集成/必要 process 验证，最后检查 Python wrapper。
使用已验证构建树与受影响 target；默认 `-j4`，不得因批次 ID 新建全仓库 build。

每批开始登记五条 coverage lane：生产入口/调用方、实现/wire、fixture/oracle、build closure、
迁移/证据；批末记录稳定可观察退出、closure decision 和 static/compile-link/runtime-test/
unobserved miss。发现同一批次新问题先修复/复审，不“静态有疑点仍进入下一批”。
编译暴露类型/链接问题并不否定静态门价值，但应回填可前置发现的原因。避免镜像实现的低价值测试。

## Required Review Techniques

1. 画出每个 mutable 字段的线程 owner、mutex 与读写点；检查锁外读、回调晚到、锁顺序和析构顺序。
2. 对每个 irreversible action 找线性化点；列出取消/超时发生在动作前、动作中、动作后的可观察结果。
3. 从每个 throw/return/callback failure 反向核对 ticket、scope、worker、pending request 和 secret 清理。
4. 沿 wire 字段追溯权威来源、签名覆盖、attempt/epoch/role/recipient 绑定，不只查字段是否存在。
5. 看完整 caller 与 surrounding code，并用现有测试反证 finding；helper 测试不能替代生产 owner 接线。
6. 核对 fixture 是否调用生产 C++ target、assertion 和 oracle 是否 C++、Face/IO 生命周期是否覆盖异步任务。
7. 区分状态恢复、失败后替换、完整上下文重算和透明 KV 迁移；不得混用名称或验收证据。

## Historical Work Mapping

| Historical work | New disposition |
| --- | --- |
| R11-B1/B2 independent authority/unary | 实现与定向证据保留；F-01 修复纳入 R12-A，按影响重跑对应出口 |
| R11-B3/B4 stream/continuation | 正例保留；线程/提交竞争转 R12-A/B；不重新设计 stream 协议 |
| R11-B5/B6/B7 recovery/replacement/cleanup | 现有受限出口保留；与 A/B 有关的竞争必须回归；未覆盖状态迁移/负例转 R12-E 对账 |
| R11-B8 caller groups | 已完成组保留，剩余与重复卡由 R12-D 的当前清单统一取代 |
| R11-B9/B10/B11 qualification/probes | 历史 PASS/失败保留；未完成出口及 fixture/trace/marker 修复归 R12-E，不再沿旧 G 编号无限增长 |
| 旧计划中先从 T001/T002 或 R11-B1 启动的文字 | SUPERSEDED；不作为当前 dispatch |

## Completion Rule

每个 R12 批次只有静态审查和其声明的定向 C++ 出口通过才可 CLOSED_FOR_VALIDATION；父任务
仍依原契约全项验收才勾选。需要外部机器、模型或运行证据的项明确 PARTIAL/NOT_RUN，不能因为
编码结束或文档结构 PASS 而关闭。任何透明 KV migration 等新增目标须用户接受后另改设计；
本次重排只修既有契约和收敛既有目标，不扩张协议。
