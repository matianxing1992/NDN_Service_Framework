# Validation Matrix

**Status**: COMPLETE. Component evidence is tracked by tasks.md; v56 supplies authoritative V13, V15, V16, V17 and V18 receipts while v12/v14/v19/v20 are bound to the same layered candidate and environment records. Historical partial negative markers remain immutable and do not replace V17.

| ID | Gate / owner | Positive or negative case | Required oracle | Current status (2026-09-11) |
| --- | --- | --- | --- | --- |
| V01 | focused unit T002/T004 | 完整 profile、异 cwd、未知/未消费字段、类型/路径逃逸、未解析 hash | 同样 resolved behavior；坏输入拒绝；build/upload/stage/sbatch spies 全 0 |
| V02 | focused unit T002 | 四库/ABI/轮子/SIF/脚本/模型/input/oracle hash 篡改、检查后替换、阶段绕过 | 指向具体缺项；失效矩阵和 stage 符合；零昂贵副作用 |
| V03 | focused unit T003/T004 | 独立 PIB/cwd/env/port、duplicate-run race、提交断联 | 不共享角色私钥；一个实际 sbatch；未知 job 状态查询而非重提 |
| V04 | focused process T003 | 部分启动、子进程早退、leader 死亡但 child 存活、signal、cleanup 写盘失败 | 自有 PID 全被回收；失败保留；无 trailing command 假绿 |
| V05 | focused app T005 | 真实权限/ACK/Selection 字段传播，结构化角色/attempt/epoch，真实输入 | 不跳过 secure DI grant；无自造 plan/ACK；迟到/错绑定不执行 |
| V06 | collector mutation T006 | 旧结果/缺字段/假 CUDA/同主机/缺 role/错边/形状/class/NaN/错误签名/timeout 冒充拒绝 | 精确关联及拒绝；正常 numerical matcher 独立于结果生成器 |
| V07 | design-code T007 | 真实 entrypoint→各字段消费者→backend→evidence→终态 | 全部 controlling gaps 关闭；不接受孤立 helper 作为接线证明 |
| V08 | build/unit T008 | 四库 source lock、工具链/Boost、真实库加载、两个扩展/实际应用、相关 unit | 干净 ABI、实际 loader/hash/version、一致解释器，所有注册单元项 exit0 |
| V09 | integration T009 | 新 Controller/角色 bootstrap 与 prepared fixture 分开；真实 CPU YOLO 多进程 | input→ACK→Selection→四阶段→response/numeric，bootstrap 就绪本身不替代 inference |
| V10 | integration T009 | 初始未授权/撤销旧 epoch、错 Selection、activation missing/tamper/late | 独立当前生产错误码/事件；超时不冒充 auth rejection；无假响应 |
| V11 | MiniNDN T010 | 正常 CPU 四角色、权限拒绝和缺依赖 cutpoint | 实际 NFD 消息/edge/role 及数值；核对源版本、命令、exit、清理 |
| V12 | base+app T011 | 基础/app分层清单、required R、全部 DSO/SONAME/RPATH、通用与应用扩展、entrypoint、只读/app与模型 | **PASS (component)** — v23 base SHA `sha256:44b44d56…6907b0` and APP manifest `sha256:4a6c3af0…942db7` are bound by the v56 candidate and transport inventory; exact composition checks pass. Execution is separately proven by V13–V18. |
| V13 | exact-SIF local T011 | 同候选 CPU 真实图及 oracle，empty home/scratch 重建 | **PASS** — v56 `tiger-local-cpu-v56-r1`, verdict `sha256:2159969ea07e52265e3147f8c28b2afe3485a078648e21cfbd83a314b3c88e89`, `NORMAL_EXPERIMENT_PASS`, 2 requests, 9 edges/request, shape `[1,50,6]`. |
| V14 | compute T012 | 实分配 host/GPU、同 Apptainer、容量、SIF/staging、双向签名 Data/服务就绪 | **PASS** — v56 allocations `210471`–`210474` on `itiger02`/`itiger03` passed compute Apptainer 1.5.3-1.el9, capacity, NFD, route, signed readiness and cleanup; see [tiger-environment](evidence/tiger-environment.md). |
| V15 | single-node T013 | 四 Provider、1 warmup+1 measured、实际模型 CUDA、Merge CPU；每个 CUDA ACK 含签名 `free_memory_mb` 资源行 | SINGLE_NODE_GPU_PASS，全图/数值/退出/清理；空/过期资源行在 placement 前 FAIL；不是跨节点证明 | **PASS** — v56 `210471` / `itiger02`, verdict `sha256:43ad2d5729798ac4f03901dbab22e80c4db261e0d72e6d37457e2fb79036d032`; model roles CUDA, Merge CPU, GPU UUID and clean cleanup recorded. |
| V16 | two-node T014 | A backbone/merge、B heads，1 warmup+3 measured | 每请求跨节点输入依赖+模型计算+数值一致；四角色/两主机/GPU 证据 | **PASS** — v56 `210472` / `tiger-two-node-gpu-v56-r1`, verdict `sha256:0972a0d69c4c866c45bf9c6228629746d3f0311b6fb74e8b5df61cfb95b4d03a`, `itiger02`/`itiger03`, 4 requests, 9 edges/request, CUDA model roles, CPU Merge, clean cleanup. |
| V17 | remote negative T015 | Selection 后所需中间 Data 缺失 | 有受控 cutpoint 证据、有限失败、零成功响应/静默重选、正常清理 | **PASS** — v56 `210473`, verdict `sha256:15cbf0df6f48e685aab3c15fdf2f0f3be90402fccc5a1f3ed0e98e575fdbd76b`, `EXPECTED_REJECTION_PASS`, exact DetectShard0→Merge cutpoint and `DEPENDENCY_DATA_MISSING`, no response/reselection, cleanup closed. |
| V18 | independent reuse T016 | 新 allocation 和身份，原正常 E/config/case 1+3 请求 | 两正常 run hashes 一致，8/8 全部成功；失败不得从统计中消失 | **PASS** — v56 `210472` and independent `210474`, same profile/base/APP/harness/model/oracle/graph, new run/allocation/GPU identities, 8/8 requests successful; verdicts `sha256:0972a0d69c4c866c45bf9c6228629746d3f0311b6fb74e8b5df61cfb95b4d03a` and `sha256:b8513ddf693b74a22d6214d19d91fe3d55ba81ff1a9454a4a3e5386f686039d6`. |
| V19 | operator T017 | 干净 checkout 获取配置/固定 artifact、离线重算、缓存复用 | **PASS** — [closure](evidence/closure.md), [tiger environment](evidence/tiger-environment.md), [integration](evidence/integration.md), guide and failure log document exact identities, commands, offline verdicts, cache and cleanup. |
| V20 | layered T002/T004/T011 | 一次app-only改动；错base/ABI、基础库遮蔽、旧layout或回执混搭 | **PASS** — v56 keeps the v23 base SHA while changing only the harness/collector plane; profile/transport inventory rejects stale paths, mode errors and mixed receipts, and exact-SIF local plus Tiger executions pass without host-library override. |

## Evidence Rules

planned report paths 位于 `specs/183-tiger-yolo-reusable-experiments/evidence/`；raw 位于 `Experiments/TigerCluster/results/<run-id>/` 或 profile 声明的 project-storage counterpart。每 receipt 带命令、exit、candidate/case、raw 索引及 hash；缺文件/未运行不能填 PASS。
单测能验证错误判定但不能证明 GPU/网络；CPU mini 模型能验证图/安全但不能证明 CUDA；单节点 GPU 能验证后端但不能证明两节点传输；两次复现不是性能统计论文。
计划内私有错误测试不把 root secret/明文激活打进日志；public cert/hash 和错误原因足以核查。策略/epoch 改动只在隔离 case 内，不能改变共享 Controller 或别人的服务。
