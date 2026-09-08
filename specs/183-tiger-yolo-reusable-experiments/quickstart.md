# Quickstart: Planned Operator Workflow

2026-09-08 **ACCEPTED / IMPLEMENTATION_PENDING**：部署目标改为基础库SIF与
外部只读app包，见[runtime layers](../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
当前命令是既有入口，尚不能把新布局参数填进去运行；T002/T004/T011须先接线。
应用变化只构建/运输变化包；基础/ABI变化才构建base。最终`local/submit`验收
绑定同一base+app+模型/harness组合，不能只检查SIF。旧完整应用镜像命令保留
为迁移前记录，不能宣称已实现分层。独立C++两节点probe209981已通过，不必重跑。

**Status**: PARTIAL。T004 已提供只读 `check`；其余命令仍是接口目标，不可执行。
尚无真实 enabled profile、合格 bundle 或模型运行结果，不能提交实验。

当前可以对已接收的 profile 运行 `check --stage inputs --profile PATH`；缺/错输入
返回 2。最低内容链通过仍返回 78（INCOMPLETE/NOT_EVALUATED），不代表允许执行。
同时提供 `--run-id/--output/--case` 可查看运行预览；不会创建输出目录。
完整边界见 [profile contract](contracts/experiment-profile.md#t004-implemented-interface-checkpoint)。

## Starting Point

工作分支 `TigerClusterExperiments`，`Experimental` 交付 `81e251a4`；运行源码锁仍为 `Experiments/TigerCluster/development-handoff.lock.json`，不自动随分支最新 commit 漂移。先读本 Spec tasks T001 和 `Experiments/TigerCluster/docs/source-handoff.md`、`docs/sif-build.md`。

## Operator Commands After Full Implementation

从仓库根执行；一份填写完整、已检查的 profile，run ID 每次新建：

```bash
python3 Experiments/TigerCluster/jobs/yolo/submit.py check --stage inputs --profile Experiments/TigerCluster/profiles/yolo-two-node.json
python3 Experiments/TigerCluster/jobs/yolo/submit.py prepare --profile Experiments/TigerCluster/profiles/yolo-two-node.json --run-id yolo-prepare-01 --output Experiments/TigerCluster/results
```

按 T007–T011 完成生产审计→unit→integration→MiniNDN→本地 SIF。只调用已有 `build-local-sif.sh`，参数从实际锁及合格 host-gate 产生；不能从此文复制一个虚构 PASS manifest。构建最多 `-j2`。

```bash
python3 Experiments/TigerCluster/jobs/yolo/submit.py local --case local-cpu --profile Experiments/TigerCluster/profiles/yolo-two-node.json --run-id yolo-local-01 --output Experiments/TigerCluster/results
python3 Experiments/TigerCluster/jobs/yolo/submit.py check --stage dispatch --profile Experiments/TigerCluster/profiles/yolo-two-node.json
```

检查通过后，在已暂存完整 immutable bundle 的 Tiger 登录目录使用同一入口提交 Slurm；计算不在 login node 执行。实际 project 位置来自 profile，下列变量由操作者设为已核对可写 project 目录：

```bash
python3 Experiments/TigerCluster/jobs/yolo/submit.py submit --case single-node-gpu --profile Experiments/TigerCluster/profiles/yolo-two-node.json --run-id yolo-gpu-01 --output "$TIGER_RESULTS_DIR"
python3 Experiments/TigerCluster/jobs/yolo/submit.py submit --case two-node-gpu --profile Experiments/TigerCluster/profiles/yolo-two-node.json --run-id yolo-two-01 --output "$TIGER_RESULTS_DIR"
python3 Experiments/TigerCluster/jobs/yolo/submit.py collect --profile Experiments/TigerCluster/profiles/yolo-two-node.json --run-id yolo-two-01 --output "$TIGER_RESULTS_DIR"
```

命令逐条执行并验证终态，不粘成自动无限队列。T015 负例使用 `negative-dependency` 独立 run；T016 再以 `two-node-gpu` 和新 run ID 提交正常配置。其他行为参数不从 shell 临时覆盖。

## Expected Result

`run.json`/candidate/各 case 记录真实节点/GPU、4角色、edge、数值、exit 和 cleanup；raw 和索引/hash 可查。两次正常结果8/8正确（2 warmup+6 measured）且 candidate/config一致、必需负例准确失败后才完成 Spec。`LOCAL_CPU_PASS`、`SINGLE_NODE_GPU_PASS` 和 `EXPECTED_REJECTION_PASS` 不能冒充正式正常双节点 PASS。

## First Failure

先查 `firstFailure`，按 input/ABI/entrypoint→identity/route→ACK→Selection→input/dependency→model→response/numeric→cleanup 定位。修改会改变候选时先按 invalidation matrix 重审/补测；不临时加 timeout、不复制旧库、不覆盖原失败。没有运行结果时报告 NOT_RUN 或 WAITING_EXTERNAL_INPUT。
