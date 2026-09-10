# Spec182 Design Audit

**Revision**: 51 | **Current source**: R11-B9-G3 current-build cross-process revalidation checkpoint on `Experimental`

## R11-B9-G3 Current-Build Cross-Process Revalidation Review 2026-09-10

本轮先复核了独立 C++ process driver 的角色 PIB/TPM、进程 ownership、cleanup、会话 parent
绑定、Provider restart 和 replacement 断言。首次运行在 Python import 的动态加载边界失败：
继承的 `LD_LIBRARY_PATH` 让不匹配的 `/usr/local/lib/libnac-abe.so` 抢先于 build 配置的
NAC-ABE 前缀，缺少 `Consumer::clearCache(...)`。该失败已作为 loader/dependency 边界保存，
没有计入协议结果；修正前缀顺序后 `ldd` 解析到匹配 NAC-ABE 安装和 NDN-SVS build。

四组独立本地运行均通过声明的 C++ 边界：conversation 首轮和 `APPEND_DELTA` 成功、错误
parent 拒绝；Provider restart 后续接安全失败且报告 `PROVIDER_CONVERSATION_STATE_MISSING`；
备用 Provider 完成 `attempt-2`；无备用 Provider 返回 `DI_NATIVE_NO_ADMITTED_PROVIDER`。
Provider 日志包含 assembly 前 grant verification 和 real ONNX Runtime CPU execution，C++
stream oracle 通过。Python 只负责进程生命周期和 fixture 文件。

该结果只提升当前构建的本地跨进程证据，不代表 Qwen3-0.6B、MiniNDN、真实 Slurm/SIF/GPU、
maintained callers、legacy zero-use、no-Python 或 T015--T017 资格。详见
[R11-B9-G3 evidence](evidence/r11-b9-g3-cross-process-current-build-20260910.md) 及
[loader failure evidence](evidence/r11-b9-g3-cross-process-dependency-boundary-20260910.md)。

## R11-B8-G44 Frozen Topology and Probe Count Integrity Review 2026-09-10

静态追踪发现 supervisor 初次渲染并保存 process map 后，后续 rank、process、port、placement
和 route 阶段仍反复读取可变的 submit-host 文件；文件替换会把不同拓扑混合到同一作业。另一个
observation evaluator 只检查 `status` 和 `closedPorts`，错误的 `reachableRoutes`、未知端口、
布尔计数或重复 route 仍可能被当作 transport PASS。exact duplicate route 还会重复 face/route
配置。

现已将已验证 map 冻结到 job scratch 的 mode-0400 副本，并让所有后续 lookup 与 route helper
只消费该副本；冻结后再次核对 scheduler order。map validation 拒绝 exact duplicate route tuple；
observation 现在严格校验 status、声明端口、非布尔整数计数及
`reachableRoutes = routeCount - len(closedPorts)`，`PASS` 必须没有 closed port。
route node rank 也必须是非 boolean 整数；畸形 rank 在拓扑错误边界 fail closed，不再泄漏为裸
Python `TypeError`。

88 个相关 Python 测试、network integration、shell/Python syntax、Spec validator 和
`git diff --check` 通过；没有 C++/ABI/SIF 变化。该修复只关闭 map TOCTOU、重复 route 和
observation-count 完整性，不推进 exact-SIF、真实 Slurm/GPU、跨节点 NDN request、no-Python
或 T016/T017 qualification。详见 [R11-B8-G44 evidence](evidence/r11-b8-g44-frozen-topology-probe-count-20260910.md)。

## R11-B8-G43 Network Probe Observation Integrity Review 2026-09-10

静态追踪发现 live `probe-multinode-network.sh` 原先把 process map 中的地址直接写入
`allocationAddresses`，因此 `evaluate_transport_probe` 的地址检查在真实路径上是自证；
同一脚本还允许 real Slurm 环境通过 `NDNSF_SPEC110_PROBE_OBSERVATION` 绕过 allocation-order
和 socket probe。另一个 map 边界允许不同节点复用同一 IP，只要端口不同，且 G42 的目录摘要
没有绑定权限位。MiniNDN 的单机文件系统、稳定 hostname 顺序和 test fixture 都不会暴露这些
分歧。

现已让 live probe 在每个目标节点执行有界 UDP route probe，记录实际选择的 source IPv4，
再按 node rank 与 map 精确比较；offline observation file 仅允许 `NDNSF_SPEC110_TEST_MODE=1`，
真实 Slurm 设置该变量会在输出创建前 fail closed。多机 map 拒绝重复 node address，观察值改为
顺序精确比较，后续每个 diagnostic `srun` 也有超时边界。`directory_digest` 现在同时绑定根和
regular-file permission bits，并拒绝 digest root symlink。

84 个相关 Python 测试、network integration、shell syntax 和 `git diff --check` 通过；没有
P0--P3 finding。该修复只关闭 observation provenance、地址唯一性、诊断超时和输入权限模式
一致性，不推进真实 Slurm/SIF/GPU、跨节点 NDN request、no-Python 或 T016/T017 qualification。
详见 [R11-B8-G43 evidence](evidence/r11-b8-g43-network-probe-observation-integrity-20260910.md)。

## R11-B8-G41 SIF Cache User Isolation Review 2026-09-10

静态追踪发现 canonical `run-container.sh` 的 node-local SIF cache 直接使用共享
`/tmp/ndnsf-di-sif-cache/<digest>`。在多用户节点上，其他用户可在 mode-755 digest
目录中预置 `stage.lock` symlink；runner 的 `exec 9>` 会先跟随并截断该目标，随后才
进入 SIF digest 校验。这是节点部署的隔离缺口，与 MiniNDN 单用户运行无关。

现已按执行 uid 分区为 `<cache-root>/u<uid>/<digest>`，并在打开 lock 前将 uid 与 digest
目录收紧为 `0700`，保持同一用户不同作业的 immutable digest 复用。SIF hash、build-record
和 Apptainer gate 均未放宽；16 个 SIF/节点脚本测试及 Shell/diff 检查通过。

该修复只关闭 cache lock 的跨用户重定向；真实共享文件系统、exact-SIF、跨节点 NDN、GPU、
no-Python 和 T016/T017 资格仍保持开放。

## R11-B8-G40 Scratch Symlink Boundary Review 2026-09-10

静态复核发现 G39 的 basename 校验仍允许一个带当前 job 名称的 symlink，或带 symlink
parent 的别名路径。preflight 可能先把 evidence 写到错误目录，topology supervisor
可能在错误目录创建 NFD socket、进程 HOME 和清理状态，随后 canonical runner 才因
`Path.resolve()` 看到另一根目录而失败；这是 MiniNDN 普通目录测试不会暴露的跨机启动分歧。

现已在 compute preflight、canonical SIF runner、topology supervisor 和直接
`render_process_launcher` 四个入口要求 `readlink -f`/`Path.resolve()` 与输入路径一致，
并拒绝 `..` scratch 别名。42 个定向测试、shell syntax 和 diff check 通过。

该修复只关闭 scratch root 的 symlink 重定向；workdir/identity 内容 digest、动态端口租约、
v1 project command 的 exact-SIF canonical runner、真实 Slurm/SIF/GPU、跨节点 NDN request、
no-Python 和 T016/T017 仍然开放。详见 [R11-B8-G40 evidence](evidence/r11-b8-g40-scratch-symlink-boundary-20260910.md)。

## R11-B8-G39 Compute Preflight Scratch Parity Review 2026-09-10

静态追踪发现 canonical `ndnsf-di.sbatch.in` 已经生成 `/tmp/ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`，
`run-container.sh` 也接受该 job-bound 后缀，但同一模板调用的
`preflight-compute.sh` 仍只接受旧的 `/tmp/${SLURM_JOB_ID}`。该模板的真实作业会在
Apptainer 启动前返回 `COMPUTE_SCRATCH_POLICY_INVALID`，而不经过 MiniNDN 或
容器 runner。现已将 preflight 收敛到与 runner/topology 相同的 basename-only
规则，并拒绝另一个 job、嵌套目录和空 basename；5 个节点脚本测试及 shell
语法检查通过。

该修复只关闭 compute preflight 的 scratch 命名接线，不证明真实 Slurm/SIF/
跨节点 NDN route、GPU、no-Python 或 T016/T017 资格。详见 [R11-B8-G39 evidence](evidence/r11-b8-g39-compute-preflight-scratch-parity-20260910.md)。

## R11-B8-G38 Network Probe Allocation Order Binding Review 2026-09-10

仓库级搜索发现 G35 保护了 supervisor 和 route retry，却遗漏了同样使用
`srun --relative=<fromNodeRank>` 的 live `probe-multinode-network.sh`。如果不绑定 Slurm
hostname 顺序，TCP/UDP diagnostic 可能从错误节点采样并产生误导性 reachability 结果。现已
在 live probe 的任何 network subprocess 前执行相同的 `scontrol show hostnames
"$SLURM_JOB_NODELIST"` 顺序检查，并显式处理查询失败；反向顺序在输出 observation 写入前
返回 `SPEC110_ALLOCATION_NODE_ORDER_MISMATCH`。network integration、25/25 topology unit、
shell syntax 和 diff check 通过。

该修复只关闭 diagnostic probe 的 scheduler-order 绑定，不证明真实 Slurm/SIF/跨节点 NDN route、
GPU、no-Python 或 T016/T017 资格。详见 [R11-B8-G38 evidence](evidence/r11-b8-g38-network-probe-allocation-order-20260910.md)。

## R11-B8-G37 Qwen Template Scratch Wiring Review 2026-09-10

静态审查发现共享的 `ndnsf-qwen.sbatch.in` 仍把 `/tmp/$USER/ndnsf-di/$SLURM_JOB_ID` 传给
canonical `run-container.sh`，与 runner 要求的 `/tmp/ndnsf-di-<SLURM_JOB_ID>` 规则不一致。
真实作业会在 Apptainer 启动前失败，而绕过该模板的 MiniNDN 不会暴露。模板现显式声明 `RUN_ID`
并生成 `/tmp/ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`；31 个 sealed-workflow tests 和 diff check
通过。

该修复只关闭共享模板的 scratch wiring，不证明真实 Slurm/SIF/跨节点 NDN route、GPU、no-Python
或 T016/T017 资格。详见 [R11-B8-G37 evidence](evidence/r11-b8-g37-qwen-template-scratch-wiring-20260910.md)。

## R11-B8-G36 Runner Scratch Name Parity Review 2026-09-10

静态审查发现批处理模板生成 `/tmp/ndnsf-di-${SLURM_JOB_ID}-${RUN_ID}`，而 canonical
`run-container.sh` 只接受无后缀的 `/tmp/ndnsf-di-${SLURM_JOB_ID}`。因此 supervisor 通过后，
runner 仍会在 Apptainer 启动前返回 `APPTAINER_SCRATCH_INVALID`；绕过 Slurm 模板的 MiniNDN
不会暴露这一分叉。现已让 runner 接受精确 job basename 或 `-<RUN_ID>` 后缀，同时保持 `/tmp/`
根和当前 `SLURM_JOB_ID` 绑定。9 个 focused runner tests、shell syntax 和 diff check 通过。

该修复只关闭入口命名契约，不证明真实 SIF/Slurm/跨节点 NDN route、GPU、no-Python 或 T016/T017
资格。详见 [R11-B8-G36 evidence](evidence/r11-b8-g36-run-container-scratch-parity-20260910.md)。

## R11-B8-G35 Scheduler Allocation Order Binding Review 2026-09-10

静态审查发现 supervisor 和独立 route retry 都使用 `srun --relative=<nodeRank>`，却没有把
process map 的 node 顺序与 Slurm allocation 的 hostname 顺序绑定。MiniNDN 的稳定创建顺序会
隐藏这一差异，真实多机可能把 NFD、identity 或 route command 派到错误节点。现已在任何 NFD
或 route command 前读取 `scontrol show hostnames "$SLURM_JOB_NODELIST"`，显式检查查询状态，
并逐项比较 rank 顺序；不一致在 pre-start 返回 `SPEC110_ALLOCATION_NODE_ORDER_MISMATCH`。
25/25 topology unit、network integration、shell syntax 和 diff check 通过。

该修复只关闭 scheduler-order preflight 边界，不证明真实 Slurm/SIF/跨节点 NDN route、GPU、
no-Python 或 T016/T017 资格。详见 [R11-B8-G35 evidence](evidence/r11-b8-g35-allocation-order-binding-20260910.md)。

## R11-B8-G34 Node Address Ownership Boundary Review 2026-09-10

静态审查发现 process map 只验证 IPv4 字面格式；多机配置若把另一台机器或错误网卡的地址填入
某个 node rank，MiniNDN 的 loopback 场景不会暴露，失败会延后到 NDN route。现已在非 test mode
启动 NFD 前，由每个对应目标节点将声明地址绑定到临时 IPv4 TCP 端口；未分配地址在 pre-start
返回 `SPEC110_NODE_ADDRESS_NOT_LOCAL`，不会启动 NFD。24/24 topology unit、network integration
和 shell syntax 通过。

该修复只确认地址归属，不证明 NDN face/route 连通、真实 Slurm/SIF/shared-storage、GPU、
no-Python 或 T016/T017 资格。详见 [R11-B8-G34 evidence](evidence/r11-b8-g34-node-address-ownership-20260910.md)。

## R11-B8-G33 Job-Scoped Scratch and Identity Root Boundary Review 2026-09-10

静态审查发现 topology supervisor 只检查 `/tmp/ndnsf-di-*` 通用前缀；真实 Slurm 作业若复用
另一作业的 scratch，G32 的 socket 清理可能误删别的作业状态。审查还发现 G31 只扫描
`identityRef/.ndn`，没有明确拒绝终端 identity root 本身为符号链接。现已在非 test mode 要求
scratch basename 绑定 `SLURM_JOB_ID`，并由 supervisor 与 launcher 双重拒绝 identity root
symlink。24/24 topology unit、network integration 和 shell syntax 通过。

该修复只收紧 scratch/identity pre-start 边界，不证明真实 Slurm/SIF/shared-storage、GPU、
no-Python 或 T016/T017 资格。详见 [R11-B8-G33 evidence](evidence/r11-b8-g33-job-scoped-scratch-identity-root-20260910.md)。

## R11-B8-G32 Stale NFD Socket Boundary Review 2026-09-10

静态审查发现 topology supervisor 只用 `test -S` 判断 NFD readiness；复用 scratch 时旧 NFD
崩溃留下的 socket 可能让已退出的新 step 被误判为 ready。现已在目标节点启动前删除 socket，
并保存 node-rank 到 `srun` PID 的映射；readiness 同时要求 PID 存活和新 socket 出现。
23/23 topology unit、network integration 和 shell syntax 通过。

该修复只收紧 NFD preflight/readiness，不证明真实 Slurm/NFD route、SIF/GPU、no-Python 或
T016/T017 资格。详见 [R11-B8-G32 evidence](evidence/r11-b8-g32-stale-nfd-socket-boundary-20260910.md)。

## R11-B8-G31 Identity Symlink Boundary Review 2026-09-10

静态审查发现 launcher 的 `cp -a` 会保留身份树中的符号链接；即使 `HOME` 已切到 scratch，
`.ndn/pib.db` 或 TPM 仍可能通过链接回到共享 `/project`。现已在 supervisor 的 pre-start
检查和直接 launcher 的 copy 前检查中拒绝 `.ndn` 树内任何 symlink。provider unit 23/23、
network integration 与 shell syntax 通过；symlink 反例在 provider/NFD 启动前分别失败。

该修复只收紧身份隔离，不证明共享存储内容 digest、Slurm/SIF/GPU、跨节点 NDN、no-Python
或 T016/T017 资格。详见 [R11-B8-G31 evidence](evidence/r11-b8-g31-identity-symlink-boundary-20260910.md)。

## R11-B8-G30 Port Availability Preflight Review 2026-09-10

静态审查确认 process map 过去只限制端口数值范围；在 `--overlap` 的 Slurm 作业中，另一份
作业可以先占用同一节点的 NFD TCP/UDP listener，旧 supervisor 会启动 NFD 后才在 readiness
阶段失败。现已拒绝 map 内重复 `(address,transport-port)` endpoint，并在任何 NFD 启动前由
目标节点执行 IPv4 TCP/UDP bind probe；注入占用反例得到 `SPEC110_PORT_NOT_AVAILABLE`、exit 4、
`survivors: 0` 且没有 NFD 日志。22/22 topology unit、network integration 和 shell syntax 通过。

这是有界 preflight，不是跨作业端口租约；probe 关闭后的竞态仍由 NFD 最终 bind 处理，动态
端口分配和真实 Slurm/SIF/跨节点资格继续开放。详见 [R11-B8-G30 evidence](evidence/r11-b8-g30-port-availability-preflight-20260910.md)。

## R11-B8-G29 Pre-Start Identity Visibility Review 2026-09-10

静态审查发现身份源原先只在 Controller/Provider/User 的 launcher `exec` 前检查；如果计算
节点未挂载某个 `/project/.../.ndn`，监督器仍会先启动全部 NFD，形成部分拓扑并把绑定错误
推迟到业务阶段。现已在首个 NFD 启动前，以每个目标节点的 `srun` 检查所有非 NFD 身份的
可读 `pib.db` 和 `ndnsec-key-file`。缺失时以 `SPEC110_IDENTITY_NOT_VISIBLE` 在 pre-start
失败，并验证 `teardown.json` 为原始 exit 4、`survivors: 0` 且无 NFD 日志。21/21 topology
unit、network integration 和 shell syntax 通过。

该修复只收紧身份输入的启动顺序，不证明真实共享存储、Slurm/SIF/GPU、跨节点 NDN、
no-Python 或 T016/T017 资格。详见 [R11-B8-G29 evidence](evidence/r11-b8-g29-prestart-identity-visibility-20260910.md)。

## R11-B8-G28 Per-Node Readiness Deadline Review 2026-09-10

静态审查发现 `run-allocation-topology.sh` 在 NFD readiness loop 外创建一个全局 30 秒
deadline。多节点部署中，前一节点的慢启动会消耗后续节点的预算；MiniNDN 单机快速启动
不会暴露这个时序耦合。现已把 deadline 创建移入每个 node row 的 loop，并在网络集成脚本
加入 source assertion 防止回退。shell syntax 与 network integration 通过，最终为
`NETWORK_SCRIPT_PASS`。

该修复只收紧预启动 readiness 计时边界，不证明真实 Slurm/SIF、跨节点 NFD route、GPU、
no-Python 或 T016/T017 资格。详见 [R11-B8-G28 evidence](evidence/r11-b8-g28-per-node-readiness-deadline-20260910.md)。

## R11-B8-G27 Routable IPv4 Address Boundary Review 2026-09-10

本轮静态审查发现 v1 topology 的地址语义比实现更宽：校验接受 IPv6、loopback、unspecified
和 multicast，但 NFD/route 代码只生成 `tcp4`/`udp4`，多机 loopback 还会指向每个节点自己的
namespace。现已要求 IPv4，拒绝 unspecified/multicast，并在 multi-node 拒绝 loopback/link-local。
21/21 topology unit、network integration 和 Python 静态检查通过。

该修复只关闭地址 preflight 分歧；端口分配、真实跨节点 route、SIF/ELF、GPU、no-Python 和
T016/T017 资格仍未运行。详见 [R11-B8-G27 evidence](evidence/r11-b8-g27-routable-ipv4-boundary-20260910.md)。

## R11-B8-G26 Host Path Command Boundary Review 2026-09-10

本轮静态审查补上了一个会让 MiniNDN 与多机部署行为分叉的命令边界：process map 原先只
检查 token 可安全解析，却允许应用参数携带 `/home`、`/project`、`/workspace`、`/build`、
`/src` 或 `/tmp` 的提交主机路径，也允许错误的 `--identity` 参数绕过进程身份复制。现已
在 map validation 和直接 launcher rendering 双入口拒绝这些 host/build 前缀；精确声明的
`identityRef` 与 NFD `--config` 仍由 launcher 绑定到 runtime HOME/job scratch。20/20
topology unit、network integration、Python/Bash 静态检查通过。

该修复只收紧 pre-exec 路径契约，不能证明 workdir 内容 digest、exact-SIF/ELF、真实 Slurm/
GPU、跨节点 NDN、no-Python 或 T016/T017 资格。详见 [R11-B8-G26 evidence](evidence/r11-b8-g26-host-path-command-boundary-20260910.md)。

## R11-B9-G2 Cross-Process Native Chain Review 2026-09-10

本轮对当前 `build-nac182` 做了 fresh 独立 C++ process revalidation：unary 数值 oracle、
stream、`FULL_CONTEXT`/`APPEND_DELTA`、alternate-provider replacement 以及无备用 Provider
的 fail-closed 终态均通过。Provider 日志同时出现 grant 在 assembly 前验证和真实 ORT CPU
执行证据；Python 只负责进程生命周期。该结果把本地 requester → Core/Authority → Provider
的主链从“已有组件”提升为可观察的跨进程出口，但不改变最终资格边界。

本轮仍使用 tiny fixture，未运行 Qwen3-0.6B、MiniNDN、真实 Slurm/SIF/GPU 或 no-Python
资格。15 个 maintained caller、legacy zero-use、T014 I02--I08 依赖反例、exact-SIF/ELF
闭包、T015--T017 仍是开放义务；详见 [R11-B9-G2 evidence](evidence/r11-b9-g2-cross-process-native-chain-20260910.md)。

## R11-B8 Multi-Machine Deployment Boundary Review 2026-09-10

本轮重新检查 `allocation_topology.py`、`run-allocation-topology.sh`、NFD 配置模板、路由/网络
probe、容器闭包门禁及 Spec110/Spec182 契约，重点寻找 MiniNDN 单机预置环境会掩盖的多机约束。
确认并修复了四类实际边界：进程身份参数可能重新打开共享 `identityRef`；Provider 只看全机
GPU UUID 而不证明 Slurm 可见设备；launcher 依赖 submit-host/evidence 路径；NFD command
可以复用固定 `/tmp` 配置且预启动失败没有 teardown 证据。当前实现已分别绑定进程 HOME/PIB/TPM、
单值 `CUDA_VISIBLE_DEVICES` + `nvidia-smi -i` UUID、目标节点 scratch launcher/config 副本，
并在 workdir/scratch/allocation/map-render/materialization 失败时记录原始退出码与
`survivors: 0`。另发现同节点 Slurm `--exclusive` 会把 NFD/Provider step 变成整节点
资源锁，现已改为 `--overlap --exact --cpus-per-task=1` 并加入禁止 exclusive 的
 fake-srun 回归。对应证据为 [G13--G20](tasks.md)、[G21](evidence/r11-b8-g21-nfd-config-scratch-20260910.md)、
[G22](evidence/r11-b8-g22-prestart-failure-evidence-20260910.md) 和
[G23](evidence/r11-b8-g23-early-input-failure-evidence-20260910.md)、[G25](evidence/r11-b8-g25-slurm-step-resource-sharing-20260910.md)。

G25 进一步修正了同节点 Slurm step 的资源语义：拓扑 supervisor、route 配置和
TCP/UDP probe 均使用 `--overlap --exact --ntasks=1 --cpus-per-task=1`，禁止
`--exclusive` 把一个长生命周期 NFD 或 Provider 变成整节点锁。fake-srun 在
integration 中直接拒绝 exclusive 参数；这只证明命令边界，不替代真实 Slurm 运行。

这些修复仍不等于多机资格。v1 process map 的 TCP/UDP 端口仍由候选输入提供，没有跨作业
端口租约或自动动态分配；启动前 bind probe 只覆盖已存在 listener，不能消除 probe 后的竞态。
G42 已按节点验证 `workdir`/identity 内容 digest；v1 生成命令也尚未
统一通过 post-Spec-111 的 exact-SIF canonical runner（Spec110 T221/T223/T225/T228）。真实
Slurm、SIF、GPU、跨节点 NDN request 和 no-Python qualification 仍属于 T014--T017/R11-B9，
不能由 fake `srun`、CPU ORT 或 MiniNDN 单机结果替代。

### R11-B8-G42 Cross-Node Input Content Digest Review 2026-09-10

静态复核发现 G20/G29 只证明每个目标节点能够访问 `workdir` 与 `identityRef`，没有证明
共享路径的内容相同。真实集群可能在不同节点挂载不同 revision；MiniNDN 的单机文件系统会把
这个分歧隐藏到请求或签名校验之后。现已加入统一的目录摘要：按根/相对目录权限位、相对普通
文件路径、普通文件权限位和文件 bytes 计算 canonical SHA-256；目标节点在任何 NFD 启动前
重新计算并与 submit-side 摘要比较，特殊文件及符号链接 fail closed。每个非 NFD identity
root 也执行同一比较，期望值写入 evidence。

受影响生产入口是 `run-allocation-topology.sh` 的 workdir/identity preflight，算法由
`allocation_topology.directory_digest` 固定；测试覆盖内容变更、符号链接和 fake `srun` 注入
的跨节点 mismatch。29/29 topology unit、network integration、shell/Python syntax 与
`git diff --check` 通过。审查无 P0--P3 finding。该修复关闭内容一致性启动门，不等于 exact-SIF、
真实 Slurm/GPU、跨节点 NDN request、no-Python 或 T016/T017 qualification；端口租约与 post-
Spec-111 v2 canonical runner 仍是开放设计边界。

## R11-B1 Independent Authority Review 2026-09-10

本轮按 Native-First Execution Order 做限定范围 source review，范围是 requester grant
composition、authority wire、独立 C++ authority entry、Waf registration、C++ tests 和
配置契约。R11-B1 已形成可构建的独立 owner 边界：requester 不再读取 authority signing
private key 或 model content key，也不构造 `NativeArtifactGrantIssuer`；authority 进程
独立持有这些 material，并在 ProviderPermission 就绪后注册 TargetedOnly service。

编译期间发现 requester 使用了未定义的 `epoch`，完整 C++ selector 又发现 local issuer
兼容构造路径使用了错误时钟；两项均已修复并通过回归。静态检查没有发现新的已确认控制
缺陷，但 Cppcheck 1.90 在 vendor nlohmann/json 与 Boost 预处理边界失败，不能作为本轮
通过证据。随后新增的 C++ process probe 与外部 launcher 通过只读静态复核；真实
Controller/Authority/requester process 正例、5 个 Authority handler 拒绝例、Authority
不可达超时和 bwrap 隔离均已运行通过。故本批结论更新为 `CLOSED_FOR_VALIDATION`（只限
R11-B1 process 出口），不推进 T005 父任务或 R11-B2 之外的生产资格。

详细五 lane、命令、原始日志和 closure decision 见 [R11-B1 evidence](evidence/r11-b1-independent-authority-20260910.md)。

## Native-First Replan 2026-09-10

用户确认的剩余顺序见 [native-first execution](contracts/native-first-execution.md)，
修订与定向一致性证据见 [replan record](evidence/native-first-replan-20260910.md)。
修正 CD-004 允许 requester 本地持 authority 私钥的部署边界，并替换 FR-019/SC-010
把所有 process tests 推迟到 T016 的规则。R11-B1/B2 是下一生产出口；R10-B83/B84
保留各自局部结果，原 16 callers、no-Python、依赖闭包和 T016 仍未关闭。
本轮是定向文档修订，不宣称重新完成全仓库源码审计。

## Current Whole-Chain Review 2026-09-10

此锚点保留跨任务全链审查的稳定链接；本轮 R11-B1 是其后的限定 authority 批次，
不替换该审查的全链结论。历史全链内容如下。

## Previous Whole-Chain Review 2026-09-10

**Revision**: 28 | **Mode**: source alignment / whole-chain static review
**Verdict**: DRAFT / PARTIAL; T016 preflight UNQUALIFIED
**Source**: `56cf6076` implementation/docs checkpoint / Experimental
**Evidence**: [R10-B84 native request identity scope](evidence/r10-b84-native-request-id-scope-20260910.md)

R10-B84 closes one native C++ identity defect found during the whole-chain review: production
`NativeInferenceClient` request names now include a fresh owner scope per native client in one final
`<scope>-<counter>` Name component, while the private test port retains deterministic IDs for native
state fixtures. The per-client construction also avoids inheriting a cached scope across `fork()`. The
new identity selector, complete `Spec182*` C++ unit selector, and 9-case native Core/Provider integration
selector pass after repair of an initial multi-component wire regression. This is a C++ production/test
result; Python remains a wrapper or oracle
and contributes no native behavior qualification here. The fix does not change the whole-Spec verdict
or advance a parent task. Independent requester/Provider worker transport, authority separation,
maintained caller/no-Python migration, and deployment dependency closure remain open.

R10-B83 closes the direct entry-point mismatch identified by R10-B82: the standalone C++ requester
now consumes the documented `conversation` object, and both it and the Python binding delegate to
one C++ loader that validates schema, requester identity, path containment, owner-only key files,
and journal construction before injecting the coordinator. This is a bounded composition result;
it does not alter the whole-Spec verdict. The large static scan found no new confirmed algorithmic
defect. It retained the existing authority-placement, 16 maintained old-route callers,
independent worker/process, no-Python, and host-bound dependency findings. The next stable exit
remains one independent requester/Provider process case with artifact identity and dependency
closure recorded.

R10-B82 reviewed the native requester and Provider entry points, Core handoff,
conversation/grant ownership, maintained callers, build registration, and dependency
closure. It found one direct contract mismatch: the standalone C++ requester does not read the
documented `conversation` configuration or inject a coordinator. It also confirmed that the
requester process currently loads the artifact-authority private key and signs grants through a
local issuer, while the design places that authority independently; this remains a production
boundary to resolve. The maintained inventory still has 16 compatibility/automatic-planner
inference calls, and no independent requester/Provider worker-process run has yet observed the
full unary/stream/continuation/recovery/cleanup chain. Current artifacts remain host-bound in
`ldd`/RUNPATH inspection. These findings keep T010/T011/T013/T014/T015/T016/T017 open or
partial. The broad static tools produced no additional confirmed algorithmic defect; local
selector/build evidence remains bounded evidence rather than full qualification.

The next stable exit is one fresh independent requester/Provider process case with artifact
identity and dependency closure recorded, after the conversation and authority boundaries are
made explicit.

**Revision**: 24 | **Mode**: source alignment / cross-task convergence
**Verdict**: DRAFT / PARTIAL; T016 preflight UNQUALIFIED
**Source**: `616657c0` implementation/docs checkpoint / Experimental
**Evidence**: [current source and dependency baseline](contracts/integrated-baseline.md)

## Current Findings

### Remaining Production Chain Review 2026-09-09

本次复核以 `3acae7ef` 为当前 source checkpoint，当前文档状态由 R10-B22 更新。R10-B1--R10-B6 已在本地关闭
准备、公开 facade、YOLO/Qwen maintained caller 的 `REPO_REF` 路由、真实 Provider 正向
消费和三个 Provider fail-closed 负例；R10-B7 同步了当前 route marker 与 T013-D/T013-F
契约文字，R10-B9 又在真实 Provider conversation fixture 中观察到 configured
`NativeInferenceClient` 发出的 `REPO_REF` Core envelope；R10-B11 又在同一真实 fixture
中观察到 Provider 以 `CollaborationContext::fetchEncryptedLargeData` 恢复并逐字节核对
该 requester-produced reference；R10-B13 修复了 Spec175 assembly fixture 的 recipe
identity 后，完整 unit 与 integration suites 均已同源通过。它们都是可独立验证的局部
出口，不等于默认 public route、跨进程或 T016 资格。

当前仍未闭合的生产主链是：真实 catalog/source → plan/offer/grant → configured native
requester → Core ACK/Selection → Provider fetch/decrypt → execution/result；单进程
requester→Provider fetch 已有局部观察，但真实 stream
callback、conversation owner/recovery、legacy zero-use 和 no-Python qualification 仍分别归
T004/T008/T010/T011/T013 与 T016。`NATIVE_REQUEST_PIPELINE_NOT_READY` 继续是无完整
runtime/configuration 构造时的显式 fail-closed 行为，不把兼容构造误认为生产成功。下一批应
外部 NFD socket 现在已可用；R10-B15 的 registration-only owner 仍在业务启动前给出
`UNQUALIFIED` / `MININDN_NODE_CONTEXT_NOT_PROVIDED`。R10-B16 已将该边界落实到 runner，
R10-B17 的显式 `--execute-owner` 又创建了 tracked requester/provider topology，并导出通过
inode、PID/start ticks、NFD socket 和 peer 校验的 node context。owner 随后因 manifest 缺少
可执行 closure case 返回 `UNQUALIFIED` / `NATIVE_CLOSURE_CASE_DEFINITION_MISSING`；下一步
需在 owner 存活期间接通 runner，并补齐 maintained caller 的跨进程执行，不把 context 生产
或静态标记当作整链完成。

### Native Closure Runner Boundary 2026-09-09

R10-B16 的只读审查覆盖 `run_case` → `make_launch`、所有当前 Python 调用点、node contract
和 23 个 focused cases。runner 现在拒绝缺失、过期或不匹配的 MiniNDN node context，持有
namespace FD 直到 `nsenter` 启动完成，并在构造命令、正常完成、超时和异常路径关闭 FD；未声明
node 的 case 不会采用偶然传入的 namespace 标记，见
[R10-B16 evidence](evidence/r10-b16-native-closure-node-context-20260909.md)。审查无
actionable finding。

该批只证明 runner 的 preflight/launch boundary；测试未执行特权 MiniNDN topology，当前 owner
仍在 `MININDN_NODE_CONTEXT_NOT_PROVIDED` 边界退出，manifest 也没有可运行的真实 DI cases。
多进程生命周期、endpoint/socket 绑定、maintained caller 两轮请求、no-Python 和 T016 资格
继续保持 `OPEN_FOR_NEXT_BATCH` / `UNQUALIFIED`。

### Native ELF and Trace Integrity Boundary 2026-09-09

R10-B18 复核了 runner 的绝对工具路径、shared-library artifact 挂载、`run_case`→`collect_trace`
调用及 trace 完整性判定。声明的 loader/libc 现在逐文件挂到 ELF 绝对路径；正常按 PID
配对的 strace `<unfinished ...>`/`<... resumed>` 不再误报，悬挂或孤立事件仍保持
`UNQUALIFIED`。root `/bin/true` probe 实际返回 0 且 observation complete，但缺少业务 evidence，
因此没有提升任何 I/PO 状态，详见 [R10-B18 evidence](evidence/r10-b18-runner-elf-trace-boundary-20260909.md)。

### Owner-to-Runner Handoff 2026-09-09

R10-B19 复核了 `main --execute-owner --runner-manifest` → `_run_owned_campaign` →
`_execute_runner_case` → canonical `run_case` 的完整接线。owner 在 MiniNDN requester/provider
namespace 和 NFD 仍存活时传入经过 inode、PID/start ticks、socket 与 peer 校验的 node context，
runner 负责 staging、launch、trace collection 和 evaluation；owner 只持久化 runner 结果并在
`finally` 清理网络。`PASS`/`FAIL`/`UNQUALIFIED` 的退出映射保持显式，默认 registration-only
入口未改变，也没有新增第二套 collector 或业务 oracle。

root run `.codex-tmp/spec182-r10-b19-20260909052040/` 的 `/bin/true` probe 返回 0，trace
`complete=true` 且 integrity/policy violations 为空，但因为没有 DI business evidence，结果为
`UNQUALIFIED` / `CANONICAL_RUNNER_RESULT_RECORDED`。29 个 focused cases、`py_compile`、
`git diff --check` 和 design validator 通过。该批只关闭 owner→runner composition boundary；
真实 native DI case、maintained caller 两轮、cross-process/no-Python 及 T016 资格仍为
`PARTIAL`/`UNQUALIFIED`，详见 [R10-B19 evidence](evidence/r10-b19-owner-runner-handoff-20260909.md)。

R10-B19 的官方 `review-agent` 只读审查无 actionable finding，覆盖 production entry/callers、
implementation/wire、test/harness/oracle、build/source closure 与 migration/evidence 五条 lane；
build lane 对 Python-only handoff 标为 `N/A`，并记录了 owner-alive root run 作为运行证据。

### Collector Evidence Boundary 2026-09-09

R10-B20 修复了 R10-B19 暴露的 evidence-generation 缺口：`collect_trace` 从实际 trace/run
推导 `identity`、`process-tree`、`namespace`、`exec-map`、`endpoints` 与 `cleanup`，而不是
把空 evidence 交给 evaluator。case 若显式声明 `businessOracle.stdoutMarker`，只有捕获的
stdout 命中该独立 marker 才补 `business-oracle`；这项 marker 不替代协议 oracle、native DI
结果或 T016 资格。

31 个 focused cases、Python compilation、design validator 与 `git diff --check` 通过。fresh
root owner/runner run 的 trace complete、returncode 0、六类运行 evidence 齐全，但 `/bin/true`
probe 没有 DI business marker，仍为 `UNQUALIFIED` / `MISSING_EVIDENCE:business-oracle`。审查
覆盖五条 required lane，无 introduced regression；build/source lane 对 Python-only collector
标记 `N/A`。T014、真实 native DI case、maintained caller、no-Python 与 T016 资格仍未关闭，详见
[R10-B20 evidence](evidence/r10-b20-collector-evidence-boundary-20260909.md)。

### I01 Native Consumer Positive Case 2026-09-09

R10-B21 使用现有同源 `build-nac182/spec182-installed-consumer` 及递归解析出的 31 项 ELF
依赖生成 candidate runner manifest，在 tracked MiniNDN owner 的 requester namespace 存活期间
执行 I01。canonical runner 通过 held namespace FD 启动 C++ consumer；returncode=0，trace
`complete=true`，integrity/policy violations 为空，六类运行 evidence 加上独立 stdout marker
`SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` 全部满足，evaluator 返回 `PASS`。

该结果证明了真实 native C++ installed-consumer 的 owner→runner→collector 闭合，不能外推为
requester/provider DI 请求、grant/selection、I02-I08 反例、maintained caller、no-Python
业务或 T016 qualification。candidate manifest 和原始输出保存在本机 `.codex-tmp/`，未把
二进制、私有依赖或机器路径提交到仓库；详见 [R10-B21 evidence](evidence/r10-b21-native-consumer-i01-pass-20260909.md)。

### R10-B22 Native DI Business Case 2026-09-09

R10-B22 在真实 `Spec182R4B6RealProviderConversation` C++ selector 的成功路径末尾加入独立
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` marker，并以系统工具链 `-j4` 重建
`integration-tests`。直接 selector rc=0、6.801 秒，marker 在第二轮 native result 断言后出现。
owner/runner 的四次新输出保留了 process-target、ELF interpreter、最小 root loader 搜索和
trust-config 缺失的首个失败边界；修正 `/lib/<SONAME>` 闭包后，测试进程在 fixture setup
因为相对 `examples/trust-any.conf` 不在最小 root 而以 returncode=201 退出，evaluator 保持
`UNQUALIFIED`。

该批证明的是隔离进程内 native requester→Core→Provider→result 的直接 business selector，
不是多进程 requester/provider transport，也不是 maintained caller、I02-I08 或 T016 资格。
manifest 目标、loader、搜索路径和 config 失败均写入 [R10-B22 evidence](evidence/r10-b22-native-di-business-case-20260909.md)，
下一批必须为 runner 增加有界的工作目录/config 绑定后用新 output 重试。

### R10-B23 Runner Working Directory and Config Boundary 2026-09-09

R10-B23 关闭了 R10-B22 首个 owner/runner setup 边界。runner 对 manifest 的
`workingDirectory` 只允许 `/probe-root` 子路径或 `/tmp`，并把声明的
`examples/trust-any.conf` data/config artifact 放入 staged root；没有增加宿主路径或任意
环境注入。官方 `review-agent` 按五条审查 lane 复核 runner、manifest 校验、launch argv、
artifact staging、测试注册与 evidence 记录，没有发现新增缺陷。

33 个 focused Python cases、`py_compile` 与设计 validator 通过。新的 root owner/runner
`PO-001` 输出目录 `.codex-tmp/spec182-r10-b23-runner-working-dir-owner/` 中，native
`Spec182R4B6RealProviderConversation` selector 返回 0，trace `complete=true`，evaluator
返回 `PASS`，七类运行 evidence 和 `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` 均满足，且无
integrity/policy violation。该结果只关闭有界的 working-directory/config harness boundary；
它仍不是多进程 requester/provider transport、maintained caller、I02-I08 或 T016 qualification。
详见 [R10-B23 evidence](evidence/r10-b23-runner-working-directory-20260909.md)。

### R10-B24 Native Suite Baseline 2026-09-09

在 R10-B23 之后复跑现有 `build-nac182` native binary：`Spec182*` unit 通过 247 个
case，`Spec182*` integration 通过 2 个 case，具名 conversation/replacement 选择通过 3 个
case，repository-reference 选择通过 1 个 case，均无错误。该批没有 native source 变更，因此没有重建；原始日志保存在
`.codex-tmp/spec182-r10-b24-native-suite-20260909/`。第二个 `vmstat` 采样出现
`si=35780`、`so=0`，按资源策略记录为换页压力；若下一次构建仍持续换页，应降至 `-j2`。

这只是同源 C++ 局部与本地 integration 基线，不代表 maintained caller、真正跨进程
requester/provider transport、I02-I08、legacy retirement、no-Python 或 T016 qualification。
详见 [R10-B24 evidence](evidence/r10-b24-native-suite-baseline-20260909.md)。

### MiniNDN Owner Context Producer 2026-09-09

R10-B17 的只读审查覆盖 `main --execute-owner`、MiniNDN constructor argv 隔离、NFD
`AppManager`/socket readiness、`collect_node_context` 的 `/proc` identity 与 topology peer
校验，以及 cleanup `finally`。26 个 focused cases、Python compilation、`git diff --check`
和文档 validator 均通过；root raw run 实际写出 requester/provider 两项 context。由于冻结
manifest 仍只有 registration，没有 runner 所需 artifact/process case，owner 返回
`NATIVE_CLOSURE_CASE_DEFINITION_MISSING`，没有启动业务进程或调用 runner。详见
[R10-B17 evidence](evidence/r10-b17-minindn-owner-context-20260909.md)。

### CrossTask Convergence 2026-09-08

R6-B3 按 T015-A 对当前生产调用链做一次整体静态收敛审查。C++
`NativeInferenceClient::request`、公开 `APPClient.request_native_payload`、维护中的
YOLO/Qwen native branches 与 `ServiceProvider` registration 均能在源码中定位；R5/R6
证据也已固定 native owner、C++ test ownership、legacy manifest 和统一 collector/harness
入口。R6-B1 将 observation integrity 与 policy violation 分开，R6-B2 将 I01--I08 与
PO-001--PO-014 注册到唯一 manifest；这些是局部出口，不是完整请求资格。

审查确认以下缺口仍是真实且有明确 owner 的未闭合项，而非文档漏记：默认 public
`distributed_inference`/ACK-driven Python routes 仍保留；维护 caller 已具备
`request_native_reference` source route，真实 fixture 已观察 Core envelope 的 `REPO_REF`
边界，但 Provider fetch/decrypt 和维护入口的完整两轮仍未完成；
stream callback 的跨进程 delivery、conversation recovery/replacement、legacy zero-use、
真实 namespace/child/socket/cleanup 和全部 PO 仍由相应实现卡或 T016 负责。
`NATIVE_REQUEST_PIPELINE_NOT_READY` 是当前显式 fail-closed 边界，不应被解释为 native
migration 完成。

没有发现新的跨任务控制性缺陷，也没有把单测、manifest 或静态状态升级为资格结论。T015-A
保持 PARTIAL，只有在上述 owner 收齐生产证据并完成 T016 真实运行后，才可进入最终 convergence
与 handoff。

### YOLO Fragment Registration Binding 2026-09-07

原生 fragment 字符串哈希遗漏注册摘要和有序节点，已改维护的规范 JSON 身份。
同时对齐 CPU/CUDA backend、1.1 余量和无 Merge 角色时的原子候选行为。实际 Python
splitter oracle 及相关 82 cases/1084 assertions PASS；case-manifest 覆盖全部当前
YOLO 用例。完整 candidate identity 与 catalog/interface 仍待闭合，见
[fragment evidence](evidence/t003-yolo-fragment-20260907.md)。

### Complete Model Descriptor Identity 2026-09-07

新增完整 AdapterDescriptor 的规范 JSON/摘要，ModelDescriptor 保留 sourceRevision
并严格绑定 adapter 兼容字段；prepare/inspect 不再仅比较简化模型字段。六组维护
Python 规范字节/摘要与替换负例通过，相关 81 cases/1067 assertions PASS。
graph adapter、splitter/candidate 全部字段及规范摘要仍待闭合，见
[descriptor evidence](evidence/t003-model-descriptor-20260907.md)。

### Candidate Node Ownership and State 2026-09-07

共享候选补 nodeRoles 与 state I/O，Qwen/YOLO 均验证全节点/全角色覆盖和依赖
无环；Qwen 三组状态精确字段对照与非法输入检查通过。新 ABI build PASS；
修复空角色测试 helper 后 69 cases/972 assertions PASS。完整 adapter descriptor、
candidate 规范摘要和实际装配映射仍缺，保持 PARTIAL，见
[node/state evidence](evidence/t003-node-state-contracts-20260907.md)。

### Shared Candidate Validation 2026-09-07

补齐 graph/model、摘要格式、ingress/egress、合法 cut/dependency tensor 集合及
rank 工件完整性校验。新校验揭示旧 SDK/native fixture 的重复 rank artifact；
修复为独立工件后 68 cases/836 assertions PASS。完整候选字段/规范摘要仍未闭合，
见 [candidate validation](evidence/t003-candidate-validation-20260907.md)。

### Graph Edges and Candidate Identity 2026-09-07

源码对照发现原生 YOLO 按节点相邻关系合成依赖、按节点数估算预算，Qwen 又哈希
既有 artifact 作为 fragment。已补真实 tensor edges、分支依赖及已知大小预算，
Qwen 保留 artifact 摘要和 onnxruntime family；新 ABI 构建及 67 cases/818 assertions
PASS。T003-A/B 撤回 DONE，完整候选 node/state/interface/identity 仍待闭合，
见 [tensor edge audit](evidence/t003-yolo-tensor-edges-20260907.md)。

### Native Core Artifact Publication 2026-09-07

新增 native publisher，作为既有 ArtifactPort 复用 Core prepared-request、加密大对象
发布和 I/O 调度。冻结 ONNX inline/external 源通过实际 native 身份核验；排队取消/
超时释放与停止后续 publication、Core 失败原因保留、真实 Core LocalMock-key 发布
均已定向检查。65 cases/779 assertions PASS；此为 native API/单元证据，不是 NAC
bootstrap、网络权限或完整 requester 资格。模型 source inspection、发布配置和
requester 主链仍待接线，见 [Core publisher](evidence/t008-core-artifact-publisher-20260907.md)。

### Publication Recertification 2026-09-07

原生已验证发布后业务 root 的原始字节及 source/initializer/model/profile 绑定，
复用既有 recipe encoder 更新 manifest/recipe，并在封存前重验原 placement 和最终
recipe 的 admitted offer 可行性。稳定工件名与 root fetch 名分开保存。
相关 58 cases/710 assertions PASS，包括真实 SDK 摘要对照和旧 exact-reuse 拒绝。
实际本地 source inspection、Core publisher 与 requester 主链仍未接通，T008-A/T004-A
保持 PARTIAL，见 [publication evidence](evidence/t008-publication-recertification-20260907.md)。

### Planning and Canonical Graph Spaces 2026-09-07

维护中的 YOLO binding 在 recipe 中使用 canonical ONNX graph，顶层保留 planning graph；
原生强制相等的校验不符合源码，现显式拆分身份并新增不同 digest 的 SDK core oracle。
新 ABI 构建与最终 58 cases/594 assertions PASS，错误摘要替换与缺失均拒绝。
同时发现 publication 后 manifest 更新与 artifact/fetch 名分离尚未由原生 owner 实现，
当前 source/manifest 前提不能直接承接维护路径。T008-A/T004-A 继续 PARTIAL，见
[graph identity evidence](evidence/t008-graph-identity-spaces-20260907.md)。

### V3 Artifact Publication 2026-09-07

ensureArtifacts/ArtifactPort 已移除旧简化 proposal 输入，直接使用候选与选定 V3 roles。
返回工件必须逐项等于角色要求，修复“合法 SHA 但对应另一工件”仍可通过的缺口。
新 ABI 构建与最终 58-case 定向验证 PASS；旧 sealer fixture 的内存预算失败与修正已留存。
真实 catalog/Repo owner、网络发布与 requester 主链未完成，T008-A 保持 PARTIAL，见
[publication evidence](evidence/t008-v3-artifact-publication-20260907.md)。

### V3 Strategy Interface 2026-09-07

placement 基类已改为完整 V3 虚接口，默认实现和只实现 V3 的自定义原生策略都通过
同一基类调用；新 ABI 构建及 58-case 定向验证 PASS。旧简化 propose 仅留在具体默认类
供迁移 fixture，不能通过策略基类回退。publication port 仍接收旧 proposal，真实
requester 尚未调用 placement；T003-C 保持 PARTIAL，见
[strategy evidence](evidence/t003-v3-strategy-interface-20260907.md)。

### V3 Sealer Connection 2026-09-07

完整 proposal/admitted offers 已直接连接 sealCore/grantView，复用准备角色和设备可行性
校验，合法 exact reuse 不再被旧 preparationAccepted 条件拒绝。CPU/GPU/multi-rank
SDK core digest 对照及相关 55-case 定向检查通过。真实 requester/catalog 与 device/dataflow
生成仍缺，T004-A 保持 PARTIAL；见 [V3 bridge evidence](evidence/t004-v3-sealer-bridge-20260907.md)。

### Inspection Source Checkpoint 2026-09-07

**A8-03 / HIGH / PARTIAL**：inspectModel 原先拼造 catalog 名称，并丢失请求完整模型描述。
已改完整 inspection 结果、expectedModel/manifest 绑定与 prepareRoles 验证；真实 catalog/Repo
owner 和 requester 主链仍缺，不能把本地 fixture 视为认证 I/O。见
[inspection evidence](evidence/t008-inspection-roles-20260907.md)。

### V3 Placement Checkpoint 2026-09-07

完整 role/rank、admitted offer 的 proposeRoles 已实现 capability/device 优先、exact
residency 与确定性成本排序；真实 SDK assignment/device 对照通过。旧简化 candidate
仍缺完整 assembly metadata，主链尚未迁移，不能用新入口的单测宣称 T003-C 或 T010 完成。
见 [V3 placement evidence](evidence/t003-v3-placement-20260907.md)。

### Offer Observation Audit 2026-09-07

**A8-02 / HIGH / PARTIAL**：policy 合成 capabilities 与缺失 offer signature 的旧入口已删除，
替换为 Core AckSelectionCandidate payload、candidate-bound policy/public-key registry 和
Ed25519 verify，真实 SDK signed fixture 对照通过。尚未接 requester 的真实订阅和完整 planner
view，T008-B 保持 PARTIAL。见 [Core offer evidence](evidence/t008-core-offer-admission-20260907.md)。
CPU 不制造资源观测，hasModel 不冒充 exact residency；网络资格仍待 T016。

### Implementation Audit 2026-09-07

以下 revision7 设计审计保留为历史；当前执行状态以 tasks 为准，不再以旧的 T001
未完成/全局 implementation BLOCK 推断现状。基线 8e86bca5，新增 **A8-01 / CRITICAL /
OPEN**：[T004 wire/identity audit](evidence/t004-wire-reopened-20260907.md)。真实
NativePlanSealer::encode 输出不满足生产 parser，且伪工件摘要、非规范 core digest、
不完整 grantView 和硬编码 projection 不能支撑完整请求。T004-A/父 T004 重开，
T010 继续完整接线前必须修复；不以本轮 13 个 Core I/O/handle 单测关闭这些义务。

### Prior Revision 7 Findings

2026-09-06追加[native capability reuse review](evidence/native-reuse-review-20260906.md)，审查源码`5239b229`。2026-09-07以[native generation contract](contracts/native-generation-design.md)补齐复用比较和采样修复语义，并移除plan旧授权句：A7-10/A7-11 CLOSED。A7-08 HIGH/OPEN仍需stream状态算法；A7-09 HIGH/OPEN已有算法处置但native修复/测试未运行，新增Top-K校验、Greedy校验与float32→double差异纳入同一T011。产品实现继续BLOCK，T001/O-004未完成；reference诊断不证明native修复。

revision 7审计已按最新源码修订文档；用户随后授权在Experimental完成182，当前T001设计收口中。已停止管理已接收交付的实验机器。
源码身份与历史状态的文档漂移已修订；未实现功能继续planned，未闭合设计继续BLOCK。
T001有界依赖探针见[native dependency design](contracts/native-dependency-design.md)，不计产品实现或T015/T016资格；后续unit/integration/MiniNDN按任务门执行，SIF/Tiger由外部负责。

| Finding / severity | Source evidence / controlling requirement | Correction / owner / closing proof |
| --- | --- | --- |
| A7-01 / HIGH / RESOLVED | spec Relationship/Assumptions与symbol readiness仍写未提交合并、integration失败；实际HEAD包含整合历史，生产路径与merge及交付源无diff；FR-015 | 更新当前baseline、分离历史checkpoint，O-001仅按源码范围CLOSED；T001仍未完成 |
| A7-02 / HIGH / RESOLVED | baseline原标VALIDATED却未包含SVS `9f2d8a47` / NDNSD `375a35c5`；当前lock和停止记录明确ABI消费者未全验证；FR-012/014，INV-008 | 固定四库pin与ABI失效范围；旧759/154结果保持历史，当前组合UNQUALIFIED；CD-009设计、T015审查、T016运行证明 |
| A7-03 / HIGH / OPEN | ServiceProvider.hpp公开addService/addCollaborationHandler，无逐服务注销；CD-014/M47需要共享宿主close语义；FR-008/010/017 | runtime-boundaries补确切接线及缺口；T001/O-004冻结registration/ACK/Selection fence、lease共享和重复注册，T009实施，PO-014检出误停共享服务与晚到工作 |
| A7-04 / MEDIUM / RESOLVED | requester仍在app_sdk/placement.py::_request_v3；NativeCanonicalOnnxAssembler.cpp::runPythonHelper与NativeStandaloneTokenizer.cpp::makeNativeStandaloneTokenizerDecoder仍启动Python；独立DI库/新facade不存在；FR-001/006/007/012 | 保持CD-001/005/006/009为planned，复用现有Provider/安全/epoch机制；T002/006/007/010及PO-001/005/006负责目标实现与证明 |
| A7-05 / MEDIUM / RESOLVED | 旧baseline链接、revision2现行声明、交付任务混入Current Checkpoint；当前root skills与Tiger交付工具已存在；FR-014/017 | 当前authority统一指integrated-baseline，旧checkpoint标历史，plan/T017复用共享技能和工具；旧Python交付模板不冒称182 no-Python成果 |
| A7-06 / HIGH / DESIGN_RESOLVED | 原Python helper有进程超时回收；ONNX checker/shape inference无取消接口，直接进程内替换会削弱deadline/cleanup；FR-008 | CD-005细化具名native worker、owned FD/进程组、steady deadline、部分输出拒绝与Provider激活fence；T006实施、T016真实证明，设计见native-onnx-assembly-design |
| A7-07 / HIGH / OPEN / CONFIRMED | 原版本独立探针：BFLOAT16 raw摘要与已知bits不符、typed正确；STRING相同模型跨进程摘要不同；FR-006/016 | 24个普通numeric稳定向量已冻结，见[identity evidence](evidence/identity-reference-20260906.json)；O-002/O-004补稳定规范和兼容处置，不复制错误/进程指针字节，未修改产品Python或原oracle |

| Open item | Controlling gap | Owner |
| --- | --- | --- |
| O-001 / CLOSED | 当前源身份、合并差异及181承接已核对；不等于当前依赖运行PASS | T001部分完成 |
| O-002 | ONNX原生装配字节契约与依赖锁 | T001 |
| O-003 / CLOSED | 精确crate/toolchain锁、C ABI/释放/串行寿命/生产路径及84对照+14负例PASS；产品迁移/隔离尚未完成 | T001设计完成；T007/T016实施证明 |
| O-004 | 12类137字段与当前源一致；完整旧能力/调用方/selectors、嵌套schema及A7-03注册寿命仍缺 | T001 |
| O-005 / CLOSED | 最小root/namespace+strace工具正反例PASS；权限/服务白名单、函数/字段、I01--I08及观测失败规则冻结；不计完整运行资格 | T001设计完成；T014/T016实施证明 |

O-002--005的有界关闭条件见[code-design](contracts/code-design.md#open-questions)，依赖探针结果与完整算法/兼容设计关闭分别记账。
T002--T014的各项实现、测试工具编写、静态审查、局部单测完成后，T015补审整体接线，
T016收齐真实运行证据，T017交付。验收标准满足即结束；变化或具体缺陷才触发受影响回归。

## Source Checks and Limits

- `git diff --name-only c770f18b HEAD -- ndn-service-framework NDNSF-DistributedInference NDNSF-DistributedRepo NDNSF-UAV-APP examples pythonWrapper wscript`与同范围`447f7584..HEAD`均无差异；读取四仓库HEAD与交付lock，核对祖先关系。tracked源码无预存改动，本地未跟踪日志/构建目录不纳入审计或提交。
- CodeGraph先查生产符号，再精确读取ServiceUser.hpp、ServiceProvider.hpp、DI_NativeProviderExecutable.cpp、两helper、examples/wscript及Python requester。宽泛结果混入`.codex-tmp/compare-*`，拒绝其作为当前源码证据，不重建索引或扫描整个临时树。
- source-field-coverage.json的12类137字段按当前Python AST核对名称/类型/默认值；这只覆盖已有表，不证明全部嵌套schema或公开调用方穷尽。现有137字段表保留，不重复建立第二份DTO权威。
- 原生Provider接线必须保留execution lease服务、V3 offer、provisioning/readiness、permission、protected preparation、epoch与结果路径；纯C++host不是只包装最终runtime.handler。Core控制publication不是通用remote-abort。
- 任务仍为17个行为单元，未因审计机械拆分。FR-001--019、SC-001--011、CD-001--014、PO-001--016及既定负例保留；实施相关unit与T016完整集成/MiniNDN的分工不变。
- Context Mode project/active健康检查通过；宽泛`status` timeline查询被guard拒绝，改用精确file-backed active tasks的relevance检索并对照源码文档。持久文件为authority，不用旧session状态裁决。

文档检查使用`check-prerequisites.sh --json --require-tasks --include-tasks`、`audit_speckit_structure.py ... --strict`、`checklists/validate_design.py`及`git diff --check`；实际结果记tasks的当前checkpoint。它们不是产品测试，也不能关闭O-002--005。

## Bounded Executor Review

用户要求使Spec182可由Spark执行；新增[执行卡](contracts/spark-execution.md)与共享技能模式，
保留17个父任务及全部FR/SC/CD/PO。未决设计继续由T001收口，Spark只领取满足依赖与设计冻结条件的实现卡。
本轮发现最新Host契约包含Core scoped registration/ExecutionLeaseService，已展开T009-A/B/C及精确源路径，
避免执行者只改DI facade而漏掉真正的代次/共享资源owner。
T006真实worker反例统一由T016执行；T006仍须交付case及纯unit，父契约和proof同步。
文档结构/路径检查不证明Spark运行效果，实际检查与边界见[spark preparation evidence](evidence/spark-execution-preparation.md)。

## History

既有审计发现的历史理由与证据保留在Git及
[revision 2](evidence/audit-revision2.md)、
[revision 3](evidence/skill-and-design-revision3.md)、
[revision 4](evidence/static-review-gate-revision4.md)、
[revision 5](evidence/adversarial-review-revision5.md)。
旧revision中固定五项风险、独立S0/S1报告与逐任务integration要求由revision 6替代。
历史源码快照与运行结果不回填为当前实现或资格证明。

## Next Action

继续按 `tasks.md` 的依赖顺序关闭剩余生产调用链：T004/T008/T009/T010/T011/T013 的
真实 requester/provider/stream/conversation 接线和 legacy zero-use 证据先由各自 owner
补齐；随后在外部 MiniNDN node/netns/NFD context 可用时，以新 run directory 重试 T016
完整 unit/integration/MiniNDN/no-Python matrix。当前 Execution Progress 为 16 个 DONE、
23 个 PARTIAL、1 个 NOT_STARTED；T016 preflight 已明确为 `UNQUALIFIED`，不能代替协议结果。
无需重开合并或续跑181资格，T017 仍依赖有效 T016 evidence。

## Progress Registry Amendment

2026-09-07：执行状态统一到 [Execution Progress](tasks.md#execution-progress)，
全部执行单元使用 [generic cards](contracts/execution-units.md)，不依赖 Spark。
旧 Spark 设计/试用记录保留历史含义；本次不改变 FR/SC、父任务验收或 Gate Order。
逐行状态为保守迁移，相关实现未经过本单元验收；检查见 [registry evidence](evidence/task-progress-registry-20260907.md)。
# A9 Placement and Artifact Binding Follow-up

2026-09-07 / OPEN：T004 已移除角色名伪工件摘要并补齐 grant 输入，33 个相关单测通过。
但 T003-C 将全部角色分配给同一 Provider，且按无关 residency 数量排序；已重开该卡与
依赖完成状态。A8-01 的 canonical wire 部分保持 OPEN。完整证据见
[binding repair and placement audit](evidence/t004-artifact-grant-bindings-20260907.md)。
## A9 Repair Verification

2026-09-07：逐角色独立 Provider、真实目标工件提示排序与预算拒绝修复定向 PASS（37 cases）。
旧 sealer fixture 依赖无关缓存加分，现已纠正；完整 proof/device/rank DTO 和 Python oracle
尚缺，A9 与 T003-C 保持 OPEN/PARTIAL。见 [placement evidence](evidence/t003-role-placement-20260907.md)。
## A8 Typed Wire Repair Verification

2026-09-07：typed shape 贯通 Selection/worker/Provider 消费者，完整 projection encoder
已通过生产 parser 往返；77 cases、2521 assertions PASS。旧 sealer 的七字段 encode 与
不完整 project 仍在原位，A8-01 保持 OPEN；不得将新 encoder 可用解释成 requester 链已闭合。
见 [typed shape and wire evidence](evidence/t004-typed-shape-20260907.md)。
## A8 Sealer Integration Verification

2026-09-07：旧七字段 encoder 和硬编码 project 已移除。完整输入通过生产 parser；core/final
摘要与真实 Python SDK oracle 一致；新 ABI 构建与 62 cases/2358 assertions PASS。
上游真实 metadata、完整 generation/device/rank oracle 及 requester 主链尚未完成，
T004 保持 PARTIAL。见 [integrated sealer evidence](evidence/t004-sealer-integrated-20260907.md)。
