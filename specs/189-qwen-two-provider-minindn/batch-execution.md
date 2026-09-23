# Spec189 Batch Execution Register

唯一成员注册表；保留历史 batch ID/证据路径，序号不再代表执行时间。
6 个活动任务；T002/T004 合并到 T003，T008/T010 合并到 T009，合并不算完成。

| Order | Batch | Members | Stable exit | Status | Unique result record |
| --- | --- | --- | --- | --- | --- |
| 1 | B189-0 | T001 | 剩余真实接线与 candidate/验证边界 | DONE (mapping only) | evidence/b189-convergence.md |
| 2 | B189-1 | T003 (former T002,T004) | 原子材料 Repo publication + source release + B189-1c audit storage gates | PARTIAL (B189-1a protected local boundary and B189-1b producer/Repo consumer locally verified; system-wide source-cache hash/size reuse observed in r155; F02/F05/F09 focused selectors pass but qualification and protected ingress remain open) | evidence/b189-prepare.md; evidence/b189-protected-range-store-20260919.md; evidence/b189-f02-memory-20260919.md; evidence/b189-f05-quota-20260919.md; evidence/b189-r155-source-cache-memory-boundary-20260920.md |
| 3 | B189-2 | T005 | ACK 后规划与生产 Selection/no-fetch fence | PARTIAL (production C++ ingress focused selectors pass; direct no-fetch counters and canonical Qwen manifest remain open) | evidence/b189-placement.md; evidence/b189-placement-production-20260919.md |
| 4 | B189-3 | T006,T007 | 范围组装、NDN handoff、C++ 输出/因果判据 | PARTIAL (T006-R1 admission sequence and T006-R2 cross-provider progress binding pass focused C++ review/selectors; r155-r162 reached real selected-material/runner boundaries; r162 was a controlled stop after authenticated entry; r163 cache-compatibility diagnostic and fresh handoff/output remain open) | evidence/b189-execution.md; evidence/b189-material-consumer-20260919.md; evidence/b189-r4-progress-heartbeat-20260919.md; evidence/b189-admission-sequence-r58-20260919.md; evidence/b189-t006-r1-progress-sequence-20260919.md; evidence/b189-r161-lineage-validation-diagnostic-20260920.md; evidence/b189-r162-cache-compatibility-stop-20260920.md; evidence/b189-r163-cache-compatibility-static-20260920.md |
| 5 | B189-5 | T009 (former T008,T010) | resource gate 后两独立 MiniNDN，各自同 handle 两请求 | BLOCKED_BY_B189-3 (must observe a post-repair assembly/terminal boundary before reuse/repeat qualification) | evidence/b189-convergence.md |

## Five-lane coverage and dynamic checks

### Target and module boundary

本 Spec 的构建边界按 Waf target 登记，不按目录名猜测。DI-only 源码变化默认使用
`ndnsf-distributed-inference` 及其直接 C++ selector/fixture；需要运行应用时显式加入
`DI_NativeRequester`、`di-native-provider`、`DI_NativeArtifactAuthority` 或
`spec189-two-provider-oracle`。Core、Repo、UAV 和其他 APP 只有在共享头文件、ABI、
生成配置或依赖闭包变化时才加入重建集合。`integration-tests` 仍按它实际链接的
递归 DI/Core closure 构建，不能手工维护一个缩小的 source subset。

使用 `scripts/install-global-target.sh --target ndnsf-distributed-inference` 做安装
边界；无范围的根 `waf install` 会重新调度整棵安装任务图，不能作为 DI 增量安装。
任务记录必须同时写明 `owner module → touched paths → Waf targets → dependent
targets → C++ selectors`，Codex 只按这份声明执行，不把“四个应用”当作自动依赖分析。

| Batch | Production/callers | Implementation/wire | Test/oracle | Build/source closure | Migration/evidence | Dynamic profile |
| --- | --- | --- | --- | --- | --- | --- |
| B189-0 | producer/consumer/handoff map | 原子材料/placement | 已有 selector 清单 | 复用 global receipt | 已验/待改/retired ID | none: docs/preflight |
| B189-1 | Runtime/requester/Repo/RepoCore error paths | manifest/schema/lease/envelope/quota/fd ownership | Repo/PreparedModel + protected receipt + T003-R1..R3 selectors | unit/integration 原闭包；混合 reservation 与 fsync/close negatives | peak/source release/quota/error ownership | owner counters; optional fixture sanitizer |
| B189-2 | Core/planner/provider ingress | signed grant/Selection | 生产 ingress negatives + placement selector | 不复制整套 DI | CPU/canonical identity/no-fetch | none: exact boundary counters |
| B189-3 | assembler/provider/coordinator + Core stream consumer | Repo material/tensor handoff and per-provider authenticated progress | assembly/endpoint/lifecycle + existing C++ oracle | 同 installable DI/Core 闭包；T006-R1/R2 selectors and fresh installed candidate | bytes/runner/output/drain | cancel/failure owner probes; small-fixture sanitizer |
| B189-5 | maintained native processes | 完整真实链 + resource gate | C++ causal/output/owner assertions | frozen tested binaries | reuse/repeat identities | host guard, 1-second samples/deadlines/stop |

## Review and closure

## Bounded execution units

能力任务不增加；B189-1 两个独立出口按子批执行，共用原证据文件，各自完成
小任务静态门、组合审查、增量构建和 C++ 测试，不为了合批跨过稳定出口。

| Sequence | Unit | Concrete exit | Explicit boundary |
| --- | --- | --- | --- |
| 1 | B189-1a / T003 | ServiceUser→Repo ciphertext commit→原 NDN 分段读取/解密；真实 package/cache eviction 后释放 serving/key lease | `spec189-encrypted-repo` 6/6、生产 Runtime protected publication 1/1、requester entrypoint；worker/cancel/key 静态门、受影响目标构建及本地 lease/range selectors 已通过；不代表 ACK/Selection、Provider 或 MiniNDN qualification |
| 2 | B189-1b / T003 | 原子层/shared manifest、protected 可达性、source release | producer 与 Repo consumer selector 已通过；protected ingress、prepare-to-assembly handoff 和真实 Qwen manifest 仍待完成；prepare 不固定两个 partition |
| 3 | B189-1c / T003 | preparation peak 分类、混合 quota reservation、filesystem fd error ownership | F02/F05/F09 focused C++ selectors passed; actual F02 ORT/RSS and protected ingress remain open and no full-model candidate is frozen |
| 4 | B189-2 / T005 | 真实 ACK/Selection/no-fetch ingress | 保留已验授权机制，仅补接线与反例 |
| 5 | B189-3 / T006-R1/R2 | 选定材料、有界 assembly、跨 Provider 认证 progress、NDN handoff、因果/独立输出 oracle | material-only consumer/预算、admission sequence 和 per-provider progress allowlist 本地接缝已验证；真实 r70 仍是旧候选的 stream-gap 边界，计数器、handoff、输出 oracle 及新候选重跑必须继续由 owner 一并交付，达到出口即验证 |
| 6 | B189-5 / T009 | 每次运行先过 host guard；两次独立 MiniNDN，各自 prepare-once/two-request、F08 generation guard、完整采样和 drain | 不扩张全局依赖迁移、SIF/Tiger 或广泛性能工程 |

B189-1a 通过只关闭共享接缝，不授予 T003 DONE；B189-1b 通过也不授予
Provider/MiniNDN PASS。资源门不再拥有独立任务状态；五 lane 继承所属批次，每个出口
记录实际覆盖文件与 selector。
The B189-3 local admission exit is stable after the sequence-state repair, and
the affected DI closure now also contains the core-vs-edge lineage validation
repair plus the explicit requester/provider cache-compatibility diagnostic path.
The shared-lineage and assembler/provider/requester changes rebuilt the DI
installable closure, not Core/Repo/UAV. r162 is a controlled stop after
authenticated entry; the old Provider-only r163 is a pre-ACK disk boundary
because prepare still publishes the large protected Repo payload. The
requester-side metadata-only seam has now passed static review, focused build,
install and CLI/linkage checks; r164 then stopped at the host admission
`diskFree` boundary before any requester/Provider process. This does not advance
T003/T006/T007/T009.

The corrected r66 command passed all binary/model digests but stopped before
MiniNDN because the root Python environment could not import the maintained
`ndn.encoding` module while deriving Provider key prefixes. Its raw run is
retained; r67 supplies the explicit pyndn/MiniNDN path and a new run ID.

r69 is a separate host preflight record: the logger-enabled retry was stopped
by `RESOURCE_BOUNDARY:diskFree` before ACK/Selection. Duplicate
initializer/payload staging was cleaned only after its logs, supervisor and
resource records were retained; the canonical source and initializer remain
unchanged.

The earlier host-native r64 and the fresh r67 both passed the resource guard,
ACK/Selection and grant verification, then stopped at
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget` before any
assembly-admission event. r60 remains a separate disk-boundary run and r61–r66
are launch/preflight records. r70 later crossed admission and selected-material
fetch with the old candidate; r155-r161 used the newer source-cache, worker-
release and lineage diagnostics and reached Provider-0 runner readiness. The
latest first boundary is the local ONNX core-lineage validation, now repaired in
the installed candidate. Do not lower the resource guard, increase timeouts, or
move T009 forward until r162 reaches a post-fix handoff/terminal boundary. This
is a scheduling constraint, not a product PASS.
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
保留 first proven boundary，修复、复审受影响范围后复测。当前最新真实边界是旧 r163
的 prepare-time `diskFree`；r30–r33 的 `swapIo` 和 r60/r163 的 `diskFree`
仍是独立宿主资源边界。`validateCore()` 修正与 requester-side metadata-only seam
已完成并安装；r164 的 `diskFreeBytes=4232839168` 低于 4 GiB 门限，必须先获得足够
磁盘空间，再以新的 run ID 运行 cache-compatibility diagnostic，才能决定下一项材料、
runner、handoff 或 terminal 修复。runner/output 尚无
证明，不能写 PASS。
