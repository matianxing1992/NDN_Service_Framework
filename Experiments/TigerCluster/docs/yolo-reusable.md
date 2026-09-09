# Reusable YOLO Distributed Inference

## Current Deployment Decision — 2026-09-08

用户确认[基础SIF + 外置DI/UAV应用](runtime-app-layers.md)，设计ACCEPTED，
迁移IMPLEMENTATION_PENDING。以下旧“完整应用SIF/九产物”描述是已有实现记录；
以Spec183最新tasks/plan为执行入口。后续app-only改动只更新独立包，不重建
基础库SIF；最终验收绑定base+app+harness/model。C++小例子209981已在itiger01/02
实跑3条Data并清理成功，基础传输证据复用。正式YOLO/GPU仍未资格化。

内部运输组件 `tools/spec183_transport.py` 已有显式inventory/receive；输入清单
只描述所选文件，不能证明完整candidate闭包。接收端先校验、测容量，再无覆盖
发布；匹配文件可复用，结果仅CONTENT_VERIFIED/NOT_EVALUATED。真实两文件SSH
验证见Spec183的 `evidence/t004-transport-receiver.md`。receive必须传与profile
一致的 `--lock-root`；未关闭/未知journal或并发提交与运输互斥。所有操作者必须
使用同一新版锁协议；旧冻结脚本不能同时参与。登录节点验证见
`evidence/t004-transport-guard.md`，跨计算节点仍待实际allocation检查。自动闭包
枚举已接入 `submit --plan-transport`，先验证现有前置门，再输出PLANNED/
NOT_EVALUATED清单（exit 78）；不上传或提交。公开submit的SSH协调已接入：
使用原始同名绝对路径、冻结bootstrap和rsync断点续传，接收端独立校验后才进入
原有唯一提交入口。真实31文件运输与复用见 `evidence/t004-ssh-coordinator.md`；
只证明登录节点运输，不替代正式前置门。新harness为27文件，当前实际profile
尚未重新冻结。`storage.transferTimeoutSeconds`默认1800秒，是总运输观察预算；
超时保留REMOTE_STATE_UNRESOLVED与暂存目录，同一run重试不得重复sbatch。

**Branch**: `TigerClusterExperiments`
**Status**: IN_PROGRESS / NOT_QUALIFIED

负例触发点已有源码和原生组件证据：`DetectShard0` 在真实V3输出校验后、
首包发布前阻止该请求到 `Merge` 的对象，并保留绑定的触发记录。见Spec183
`evidence/t004-dependency-cutpoint.md`。负例User终态、双rank接线与collector仍缺，
公开提交仍拒绝NEGATIVE_RUNNER_NOT_WIRED；不能手工移除该保护来启动GPU。

目标：一份profiles/yolo-two-node.json和一个jobs/yolo/submit.py入口，本地验证后，以同一完整SIF完成Tiger两节点四Provider推理并在新allocation复现。当前有实际输入 profile 和历史 base SIF，尚无本候选合格 SIF/GPU allocation；共享目录接收端submit已接，尚不具备真实提交资格。

## Current Checkpoint

Spec183 T001输入/接口清点完成。后续已接收锁定 source seal、本地历史 base SIF
及 YOLO package；签名 catalogue、graph/weights 和 reference 读取验证见
[输入验证](../../../specs/183-tiger-yolo-reusable-experiments/evidence/yolo-input-validation.md)。
这不等于当前应用 SIF 已合格，不能用历史 base 或旧 tiny-onnx 结果冒充 YOLO 资格。
正常YOLO User是一次请求入口；warmup/measured分别执行和保存，不假定legacy sequential参数在当前路径有效。

`prepare` 目前只冻结通过内容检查的脚本/计划，返回 PREPARED/NOT_EVALUATED。
宿主侧 `runtime.yolo_operator.provision_run` 已能有界调用现有容器内离线 issuer，
验证返回文件并保留进程清理记录；它是内部调用边界，不是新的公开命令。
local 已连接 profile 输入、私钥 locator、冻结 bundle 和 SIF worker：消费同源
hostMinindn receipt 后执行签发、两个 CPU 请求、清理及 collector 重算。缺门或
错误的 native manifest 在启动前拒绝。v52 已用 v32 APP + v22 base SIF 完成一次
真实 exact-SIF MiniNDN Y-B；v51 的 Y-N-D 也已从同一 APP 的保留日志重算出
post-Selection 缺依赖证据。正常单/双GPU run、节点scratch、外部
collect --reconcile 和共享目录接收端submit已接；跨机器文件运输、可移植前置
证据和负例仍缺。新机器必须安装冻结 requirements-operator.txt 对应的操作者
依赖并保证batch解释器一致。Tiger已建立独立环境，当前profile的
runtime.operatorPython指向它；系统Python仍不作为该环境的替代。

## Direct local YOLO example

要向其他人演示构建物，使用固定 base SIF，并把独立 APP bundle 以只读方式挂载到
`/app`。完整的已实跑命令、SHA256、数值回执和清理证据见
[v52 exact-SIF evidence](../../../specs/183-tiger-yolo-reusable-experiments/evidence/minindn-v52-exact-sif-yb-v32.md)。
最小入口如下（`RUN` 必须是新建的、不可复用的 run ID）：

```bash
ROOT=$(readlink -f Experiments/TigerCluster/.cache/layered-base-20260909)
RUN=minindn-local-<date>-<id>
OUT=Experiments/TigerCluster/results
export SPEC180_RUNTIME_SIF="$ROOT/base-runtime-controller-version-j4-v22.sif"
export SPEC180_RUNTIME_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer
export SPEC180_RUNTIME_APP_ROOT="$ROOT/app-controller-version-j4-v32"
export PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper:$PWD/pythonWrapper"

# Freeze one new run; prepare intentionally exits 78/NOT_EVALUATED.
python3 Experiments/TigerCluster/jobs/yolo/submit.py prepare \
  --profile Experiments/TigerCluster/profiles/yolo-two-node-controller-v32.json \
  --run-id "$RUN" --output "$OUT" --case local-cpu || test $? -eq 78
python3 Experiments/TigerCluster/tools/spec183_dev_provision.py provision \
  --profile Experiments/TigerCluster/profiles/yolo-two-node-controller-v32.json \
  --run-id "$RUN" --output "$OUT" --apptainer "$SPEC180_RUNTIME_APPTAINER"

# The provision step writes public/preparation.json and its SHA is an input.
PREP=$(sha256sum "$OUT/$RUN/public/preparation.json" | awk '{print "sha256:"$1}')
python3 -u Experiments/TigerCluster/tools/spec183_minindn.py \
  --run-id "$RUN" --output "$OUT" \
  --profile Experiments/TigerCluster/profiles/yolo-two-node-controller-v32.json \
  --preparation-sha256 "$PREP" --case Y-B
```

`spec183_minindn.py` starts the registered four-provider MiniNDN graph; the
provider/controller binaries come from the APP bundle and stable libraries
come from the SIF.  Do not replace the APP with host binaries or inject host
libraries.  With `SPEC180_RUNTIME_SIF` set, the child processes receive the
SIF-owned library path; leave `SPEC180_HOST_LIBRARY_PATH` unset unless the host
operator has a separately verified matching closure.  This command
demonstrates local CPU inference; it does not claim Tiger GPU qualification.

提交回执丢失时使用同一run ID重试submit，只会按唯一comment查询原job，不重提。
默认collect离线重算；作业结束后显式collect --reconcile核对scheduler终态并释放
journal。共享输出路径不能手动改写prepared receipt；目前必须先完成运输接线。

## Operator environment

操作者依赖不安装进SIF。当前Tiger环境为
`/project/tma1/ndnsf-di/operator-envs/py39-3b4f62bc/bin/python`；依赖文件摘要为
`3b4f62bcad8e182c7402f9068dce192afd3283be0b9fc5ec2d0e597aa7a9b755`。
新环境用维护的工具创建（prefix父目录须已存在）：

```bash
/usr/bin/python3 Experiments/TigerCluster/tools/spec183_operator_env.py \
  --prefix /project/tma1/ndnsf-di/operator-envs/py39-3b4f62bc \
  --requirements Experiments/TigerCluster/requirements-operator.txt
```

重复调用只验证并复用完成的环境；不会重装或清空失败/正在安装的prefix。
集群上使用该环境的Python调用submit.py；batch通过已绑定参数、srun通过已验证
profile选择同一解释器。submit协调、本地CPU和普通离线collect使用调用者本机解释器，进入
冻结CLI后也核对实际依赖pins。显式collect --reconcile使用集群解释器。
每个实际allocation仍需证明compute节点可加载该环境；登录节点通过不代表GPU通过。

## Operating Contract

- Tiger专用脚本/配置在本树；Core/DI/Repo实现保留原owner。
- 输入以[development lock](../development-handoff.lock.json)为准；实验分支commit与运行库commit分开。
- 顺序：实现/生产审计→unit→integration→MiniNDN→exact-SIF CPU→单机GPU→双机GPU→新allocation复现。
- check/prepare/local/submit/collect拒绝未知字段、错hash、缺门；check不得构建/上传/SSH/sbatch。
- GPU模型角色禁止静默CPU fallback，Merge明确CPU后处理。BackboneNeck/DetectShard0/DetectShard1/Merge放置A/B/B/A。
- 每请求记录request/attempt/plan、跨节点边、backend、数值和子进程清理；READY/响应非空/exit0单独不足。
- 正常两节点只有在两个 rank 都返回绑定的 `node-receipt.json` 后，才由
  `runtime.yolo_operator.finalize_normal_collection()` 发布一次不可覆盖的
  `collection-input.json`；它使用 `applicationName + '/sync'`，不使用 legacy
  `/group`，也不从缺失文件或退出码推断 PASS。
- 最大-j4；SIF本地构建、Tiger只验证/运行；CAS缓存不按run复制模型，容量按实际峰值检查。

真实参数、命令和成功示例见 [Spec183 evidence](../../../specs/183-tiger-yolo-reusable-experiments/evidence/minindn-v52-exact-sif-yb-v32.md)；未运行继续NOT_RUN，不复制历史PASS。计划见[Spec183](../../../specs/183-tiger-yolo-reusable-experiments/plan.md)。
