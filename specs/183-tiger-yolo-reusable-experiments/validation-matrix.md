# Validation Matrix

**Status**: Component implementation evidence is tracked by the detailed table in tasks.md. Formal runtime qualification remains NOT_RUN; component checks do not close the runtime gates below.

| ID | Gate / owner | Positive or negative case | Required oracle |
| --- | --- | --- | --- |
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
| V12 | base+app T011 | 基础/app分层清单、required R、全部 DSO/SONAME/RPATH、通用与应用扩展、entrypoint、只读/app与模型 | 精确组合内验证；显式CUDA driver例外，旧app/错base/host prefix/基础库遮蔽拒绝；不能只测host import |
| V13 | exact-SIF local T011 | 同候选 CPU 真实图及 oracle，empty home/scratch 重建 | LOCAL_CPU_PASS；记录 CPU 与 GPU case 不同，无 GPU 资格推断 |
| V14 | compute T012 | 实分配 host/GPU、同 Apptainer、容量、SIF/staging、双向签名 Data/服务就绪 | 两节点实际版本/route/permission；失配在 Provider 前失败 |
| V15 | single-node T013 | 四 Provider、1 warmup+1 measured、实际模型 CUDA、Merge CPU；每个 CUDA ACK 含签名 `free_memory_mb` 资源行 | SINGLE_NODE_GPU_PASS，全图/数值/退出/清理；空/过期资源行在 placement 前 FAIL；不是跨节点证明 |
| V16 | two-node T014 | A backbone/merge、B heads，1 warmup+3 measured | 每请求跨节点输入依赖+模型计算+数值一致；四角色/两主机/GPU 证据 |
| V17 | remote negative T015 | Selection 后所需中间 Data 缺失 | 有受控 cutpoint 证据、有限失败、零成功响应/静默重选、正常清理 |
| V18 | independent reuse T016 | 新 allocation 和身份，原正常 E/config/case 1+3 请求 | 两正常 run hashes 一致，8/8 全部成功；失败不得从统计中消失 |
| V19 | operator T017 | 干净 checkout 获取配置/固定 artifact、离线重算、缓存复用 | 无个人路径/插件依赖；结果可再判定；没有重复 SIF/模型复制 |
| V20 | layered T002/T004/T011 | 一次app-only改动；错base/ABI、基础库遮蔽、旧layout或回执混搭 | app源/产物/E更新，R/SIF hash不变；只编译受影响目标；拒错零workload/上传/提交；新组合实际import/入口通过；PLANNED |

## Evidence Rules

planned report paths 位于 `specs/183-tiger-yolo-reusable-experiments/evidence/`；raw 位于 `Experiments/TigerCluster/results/<run-id>/` 或 profile 声明的 project-storage counterpart。每 receipt 带命令、exit、candidate/case、raw 索引及 hash；缺文件/未运行不能填 PASS。
单测能验证错误判定但不能证明 GPU/网络；CPU mini 模型能验证图/安全但不能证明 CUDA；单节点 GPU 能验证后端但不能证明两节点传输；两次复现不是性能统计论文。
计划内私有错误测试不把 root secret/明文激活打进日志；public cert/hash 和错误原因足以核查。策略/epoch 改动只在隔离 case 内，不能改变共享 Controller 或别人的服务。
