# Spec189 Batch Execution Register

唯一成员注册表；保留历史 batch ID/证据路径，序号不再代表执行时间。
7 个活动任务；T002/T004 合并到 T003，T010 合并到 T009，合并不算完成。

| Order | Batch | Members | Stable exit | Status | Unique result record |
| --- | --- | --- | --- | --- | --- |
| 1 | B189-0 | T001 | 剩余真实接线与 candidate/验证边界 | DONE (mapping only) | evidence/b189-convergence.md |
| 2 | B189-4 | T008 | full-model 前 guard/受控 stop/drain | PARTIAL (guard/lifecycle checked; live counters pending) | evidence/b189-resource.md |
| 3 | B189-1 | T003 (former T002,T004) | 原子材料 Repo publication + 同 handle reuse | PARTIAL | evidence/b189-prepare.md |
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
