# Spec189 Batch Execution Register

唯一成员注册表；保留历史 batch ID/证据路径，序号不再代表执行时间。
7 个活动任务；T002/T004 合并到 T003，T010 合并到 T009，合并不算完成。

| Order | Batch | Members | Stable exit | Status | Unique result record |
| --- | --- | --- | --- | --- | --- |
| 1 | B189-0 | T001 | 剩余真实接线与 candidate/验证边界 | DONE (mapping only) | evidence/b189-convergence.md |
| 2 | B189-4 | T008 | full-model 前 guard/受控 stop/drain | PARTIAL (safety entry closed; live counters owned by T003/T006 and pending) | evidence/b189-resource.md |
| 3 | B189-1 | T003 (former T002,T004) | 原子材料 Repo publication + 同 handle reuse | PARTIAL (B189-1a closed; B189-1b producer/Repo consumer verified, protected ingress open) | evidence/b189-prepare.md |
| 4 | B189-2 | T005 | ACK 后规划与生产 Selection/no-fetch fence | PARTIAL | evidence/b189-placement.md |
| 5 | B189-3 | T006,T007 | 范围组装、NDN handoff、C++ 输出/因果判据 | PARTIAL | evidence/b189-execution.md |
| 6 | B189-5 | T009 (former T010) | 两独立 MiniNDN 成功，各自同 handle 两请求 | PARTIAL | evidence/b189-convergence.md |

## Five-lane coverage and dynamic checks

| Batch | Production/callers | Implementation/wire | Test/oracle | Build/source closure | Migration/evidence | Dynamic profile |
| --- | --- | --- | --- | --- | --- | --- |
| B189-0 | producer/consumer/handoff map | 原子材料/placement | 已有 selector 清单 | 复用 global receipt | 已验/待改/retired ID | none: docs/preflight |
| B189-4 | worker/runner owner | cancel/drain/stop | C++ lifecycle + host guard | 仅受影响目标 | resource stop/cleanup | bounded stop; small-fixture ASan/UBSan if supported |
| B189-1 | Runtime/requester/Repo | manifest/schema/lease/envelope | 已有 Repo/PreparedModel + real receipt | unit/integration 原闭包 | 冷热/source release | owner counters; optional fixture sanitizer |
| B189-2 | Core/planner/provider ingress | signed grant/Selection | 生产 ingress negatives + placement selector | 不复制整套 DI | CPU/canonical identity/no-fetch | none: exact boundary counters |
| B189-3 | assembler/provider/coordinator | Repo material/tensor handoff | assembly/endpoint/lifecycle + existing C++ oracle | 同 installable DI 闭包 | bytes/runner/output/drain | cancel/failure owner probes; small-fixture sanitizer |
| B189-5 | maintained native processes | 完整真实链 | C++ causal/output assertions | frozen tested binaries | reuse/repeat identities | 1-second samples/deadlines/stop |

## Review and closure

## Bounded execution units

能力任务不增加；B189-1 两个独立出口按子批执行，共用原证据文件，各自完成
小任务静态门、组合审查、增量构建和 C++ 测试，不为了合批跨过稳定出口。

| Sequence | Unit | Concrete exit | Explicit boundary |
| --- | --- | --- | --- |
| 1 | B189-4 safety entry | 已验 host guard/受控停止与小型 lifecycle | 新 native counters 随所属组件交付；T009 前补齐，T008 暂不勾选；不再为 counters 创建重复任务 |
| 2 | B189-1a / T003 | ServiceUser→Repo ciphertext commit→原 NDN 分段读取/解密；真实 package/cache eviction 后释放 serving/key lease | `spec189-encrypted-repo`、既有 bounded publisher/Runtime selectors；不加入 atomic schema/ORT/handoff；worker/cancel/key 静态门、组合构建及 14 个 C++ selectors 已通过 |
| 3 | B189-1b / T003 | 原子层/shared manifest、protected 可达性、source release、同 handle 零发布 | producer 与 Repo consumer selector 已通过；protected ingress、prepare-to-assembly handoff 和真实 Qwen manifest 仍待完成；prepare 不固定两个 partition |
| 4 | B189-2 / T005 | 真实 ACK/Selection/no-fetch ingress | 保留已验授权机制，仅补接线与反例 |
| 5 | B189-3 / T006,T007 | 选定材料、有界 assembly、NDN handoff、因果/独立输出 oracle | 计数器在 owner 一并交付，达到出口即验证 |
| 6 | B189-5 / T009 | 全部计数器接入后两次独立 MiniNDN，各自 prepare-once/two-request | 不扩张全局依赖迁移、SIF/Tiger 或广泛性能工程 |

B189-1a 通过只关闭共享接缝，不授予 T003 DONE；B189-1b 通过也不授予
Provider/MiniNDN PASS。五 lane 继承 B189-1，每个出口记录实际覆盖文件与 selector。
使用一个 Repo 后端和一个 Core protected serving 路径；不新增明文网络兼容分支、
Qwen 特供协议或第二套缓存淘汰策略。

### Review procedure

逐任务提供 ID、design binding、batch base、精确 diff 与五 lane；
冻结待审范围，或制作包含相关 untracked 文件的不可变快照，不混入无关脏文件。
官方 review-agent 只读，修复后复审受影响不变量；同批最后组合审查。
主代理核对 actual tested candidate 与 snapshot，再统一增量构建/C++ 测试。

唯一结果记录包含实际 selector/命令/并发/耗时、快照、动态检查适用性、
static / compile-link / runtime-test / unobserved 漏检和 closure decision。
达到出口即测试，不为字段或日志单独增加行政任务，不无限扩批。

## Retry and resource gate

T008 前置所有 full-model prepare/MiniNDN，不能在执行后补录。
遵守 [retry loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md)；
保留 first proven boundary，修复、复审受影响范围后复测。
当前 r25 FAIL 已观察 assembly entry，runner/output 尚无证明，不能写 PASS。
