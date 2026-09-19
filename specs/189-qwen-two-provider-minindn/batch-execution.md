# Spec189 Batch Execution Register

唯一成员注册表；保留历史 batch ID/证据路径，序号不再代表执行时间。
6 个活动任务；T002/T004 合并到 T003，T008/T010 合并到 T009，合并不算完成。

| Order | Batch | Members | Stable exit | Status | Unique result record |
| --- | --- | --- | --- | --- | --- |
| 1 | B189-0 | T001 | 剩余真实接线与 candidate/验证边界 | DONE (mapping only) | evidence/b189-convergence.md |
| 2 | B189-1 | T003 (former T002,T004) | 原子材料 Repo publication + source release + B189-1c audit storage gates | PARTIAL (B189-1a closed; B189-1b producer/Repo consumer locally verified; F02/F05/F09 and protected ingress open) | evidence/b189-prepare.md; evidence/di-repo-design-static-audit-20260919.md |
| 3 | B189-2 | T005 | ACK 后规划与生产 Selection/no-fetch fence | PARTIAL | evidence/b189-placement.md |
| 4 | B189-3 | T006,T007 | 范围组装、NDN handoff、C++ 输出/因果判据 | PARTIAL (material-only consumer local exit passed; handoff/output open) | evidence/b189-execution.md; evidence/b189-material-consumer-20260919.md |
| 5 | B189-5 | T009 (former T008,T010) | resource gate 后两独立 MiniNDN，各自同 handle 两请求 | PARTIAL | evidence/b189-convergence.md |

## Five-lane coverage and dynamic checks

| Batch | Production/callers | Implementation/wire | Test/oracle | Build/source closure | Migration/evidence | Dynamic profile |
| --- | --- | --- | --- | --- | --- | --- |
| B189-0 | producer/consumer/handoff map | 原子材料/placement | 已有 selector 清单 | 复用 global receipt | 已验/待改/retired ID | none: docs/preflight |
| B189-1 | Runtime/requester/Repo/RepoCore error paths | manifest/schema/lease/envelope/quota/fd ownership | Repo/PreparedModel + protected receipt + T003-R1..R3 selectors | unit/integration 原闭包；混合 reservation 与 fsync/close negatives | peak/source release/quota/error ownership | owner counters; optional fixture sanitizer |
| B189-2 | Core/planner/provider ingress | signed grant/Selection | 生产 ingress negatives + placement selector | 不复制整套 DI | CPU/canonical identity/no-fetch | none: exact boundary counters |
| B189-3 | assembler/provider/coordinator | Repo material/tensor handoff | assembly/endpoint/lifecycle + existing C++ oracle | 同 installable DI 闭包 | bytes/runner/output/drain | cancel/failure owner probes; small-fixture sanitizer |
| B189-5 | maintained native processes | 完整真实链 + resource gate | C++ causal/output/owner assertions | frozen tested binaries | reuse/repeat identities | host guard, 1-second samples/deadlines/stop |

## Review and closure

## Bounded execution units

能力任务不增加；B189-1 两个独立出口按子批执行，共用原证据文件，各自完成
小任务静态门、组合审查、增量构建和 C++ 测试，不为了合批跨过稳定出口。

| Sequence | Unit | Concrete exit | Explicit boundary |
| --- | --- | --- | --- |
| 1 | B189-1a / T003 | ServiceUser→Repo ciphertext commit→原 NDN 分段读取/解密；真实 package/cache eviction 后释放 serving/key lease | `spec189-encrypted-repo`、既有 bounded publisher/Runtime selectors；不加入 atomic schema/ORT/handoff；worker/cancel/key 静态门、组合构建及 14 个 C++ selectors 已通过 |
| 2 | B189-1b / T003 | 原子层/shared manifest、protected 可达性、source release | producer 与 Repo consumer selector 已通过；protected ingress、prepare-to-assembly handoff 和真实 Qwen manifest 仍待完成；prepare 不固定两个 partition |
| 3 | B189-1c / T003 | preparation peak 分类、混合 quota reservation、filesystem fd error ownership | F09 focused C++ selector passed; F02/F05 remain open and no full-model candidate is frozen |
| 4 | B189-2 / T005 | 真实 ACK/Selection/no-fetch ingress | 保留已验授权机制，仅补接线与反例 |
| 5 | B189-3 / T006,T007 | 选定材料、有界 assembly、NDN handoff、因果/独立输出 oracle | material-only consumer/预算已验证；计数器、handoff、输出 oracle 在 owner 一并交付，达到出口即验证 |
| 6 | B189-5 / T009 | 每次运行先过 host guard；两次独立 MiniNDN，各自 prepare-once/two-request、F08 generation guard、完整采样和 drain | 不扩张全局依赖迁移、SIF/Tiger 或广泛性能工程 |

B189-1a 通过只关闭共享接缝，不授予 T003 DONE；B189-1b 通过也不授予
Provider/MiniNDN PASS。资源门不再拥有独立任务状态；五 lane 继承所属批次，每个出口
记录实际覆盖文件与 selector。
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

Host guard 前置所有 full-model prepare/MiniNDN，不能在执行后补录。
遵守 [retry loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md)；
保留 first proven boundary，修复、复审受影响范围后复测。
当前 r27 是 canonical identity 的资源边界；r25 仍只作历史 post-grant 参考，
runner/output 尚无证明，不能写 PASS。
