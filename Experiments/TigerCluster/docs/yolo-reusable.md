# Reusable YOLO Distributed Inference

## Current Deployment Decision — 2026-09-08

用户确认[基础SIF + 外置DI/UAV应用](runtime-app-layers.md)，设计ACCEPTED，
迁移IMPLEMENTATION_PENDING。以下旧“完整应用SIF/九产物”描述是已有实现记录；
以Spec183最新tasks/plan为执行入口。后续app-only改动只更新独立包，不重建
基础库SIF；最终验收绑定base+app+harness/model。C++小例子209981已在itiger01/02
实跑3条Data并清理成功，基础传输证据复用。历史 APP v35 + v22 base 已完成 exact-SIF
本机/host gate、Tiger 单节点 GPU `210340` 以及首个双节点正常 `210341`；完整
可复用交付仍未资格化，因为负例 `210342` 暴露了 completion budget 和逻辑 edge
cardinality 两个 harness 缺陷，T016 尚未运行。

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
只证明登录节点运输，不替代正式前置门。新harness为27文件；当前 v48 profile
快照已绑定 v39 APP、v22 base 和 v40 host receipt。`storage.transferTimeoutSeconds`默认1800秒，是总运输观察预算；
超时保留REMOTE_STATE_UNRESOLVED与暂存目录，同一run重试不得重复sbatch。

**Branch**: `TigerClusterExperiments`
**Status**: IN_PROGRESS / LOCAL_PUBLICATION_BLOCKED / NEGATIVE_HARNESS_BLOCKED

## 2026-09-10 current execution update

The current candidate is APP v39 over the rebuilt v23 base SIF. The v23 base
SHA is `sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`
(3,586,351,104 bytes); APP v49 manifest SHA is
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`.
The APP is an external read-only bundle. Base and APP builds are bounded to
`-j4`; the GCC9 pybind fallback is `-O0 -g0 -B/usr/bin/` with
`BOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES`.

Direct real MiniNDN Y-B/Y-N remain `PASS`. A clean formal local owner run then
passed with the exact same composition:

```bash
python3 Experiments/TigerCluster/jobs/yolo/submit.py prepare \
  --profile /project/tma1/ndnsf-di/candidates/spec183-v49-20260910/profile-v49.json \
  --run-id tiger-local-cpu-v49-r1 \
  --output /project/tma1/ndnsf-di/runs --case local-cpu || test $? -eq 78
python3 Experiments/TigerCluster/jobs/yolo/submit.py local \
  --profile /project/tma1/ndnsf-di/candidates/spec183-v49-20260910/profile-v49.json \
  --run-id tiger-local-cpu-v49-r1 \
  --output /project/tma1/ndnsf-di/runs --case local-cpu
```

The verdict is `NORMAL_EXPERIMENT_PASS`, candidate digest
`sha256:ed7967c65e6c765f74f4ad64fd45154b5094dbdc380a03a49a75eee0bd985384`,
two requests, nine dependency edges per request, shape `[1,50,6]`,
`matched=true`, and `maxAbsError=0.0005340576171875`. The retained file is
`/project/tma1/ndnsf-di/runs/tiger-local-cpu-v49-r1/verdict.json` with SHA
`sha256:d15b41c1e853584de893d996741b31d9ee95b4ec89804e0373ad623486c17ebe`.

The prior v110 publication callback failure, cache/extraction mistakes, stale
NFD selection, and the first remote candidate-root transport failure remain
immutable diagnostic records. The v49 Tiger order is formal local PASS →
single-node GPU → fresh two-node normal → negative T015 → independent normal
reuse T016. Submission `tiger-single-node-gpu-v49-r7` is currently transferring
the same candidate; no remote GPU PASS is claimed until Slurm/CUDA/cleanup
receipts are collected. Full hashes and retained failures are in the
[Spec183 checkpoint](../../../specs/183-tiger-yolo-reusable-experiments/evidence/tiger-runtime-checkpoint-20260910.md).

负例触发点已有源码和原生组件证据：`DetectShard0` 在真实V3输出校验后、
首包发布前阻止该请求到 `Merge` 的对象，并保留绑定的触发记录。见Spec183
`evidence/t004-dependency-cutpoint.md`。当前远端负例 `210342` 已真实走到
Selection/withheld，但因 User completion budget 和逻辑 edge cardinality 缺陷未
形成 collector 终态；不能手工移除保护或把部分日志标为 PASS。

目标：一份profiles/yolo-two-node.json和一个jobs/yolo/submit.py入口，本地验证后，以同一分层 base+APP 组合完成Tiger两节点四Provider推理并在新allocation复现。历史 v35 候选已有单节点和首个双节点正常 PASS；当前 v39 候选仍等待正式 local publication 修复、负例和第二次正常 allocation。

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
collect --reconcile 和共享目录接收端submit已接；跨机器文件运输和可移植前置
证据已在实际 allocation 使用。负例 `210342` 仍缺 User/collection 终态：外层
60 秒预算覆盖不了 60 秒观察加 shutdown，且生产图有两个同源 DetectShard0→Merge
逻辑 edge。新机器必须安装冻结 requirements-operator.txt 对应的操作者
依赖并保证batch解释器一致。Tiger已建立独立环境，当前profile的
runtime.operatorPython指向它；系统Python仍不作为该环境的替代。

历史 APP v35 已完成 host gate、exact-SIF MiniNDN Y-B/Y-N，以及
共享 `tiger-local-cpu-v35` 的 `NORMAL_EXPERIMENT_PASS`。它复用内容锁定的
v22 base SIF，只重建外置 APP 和受影响 planes。Tiger single-node GPU
`210340`（startup=300）完成 1+1；首个 two-node normal `210341`
（`itiger02`/`itiger03`，1 warmup + 3 measured）完成四角色 backend、9 条依赖
边/请求、数值与清理闭环。负例 `210342` 通过 Selection、GPU/provider readiness
并在 DetectShard0 记录两个 bound withheld，但 User 在 59.9946 秒外层预算被杀，
没有 `negative-user.json`；collector 还把同源两条逻辑 edge 当成单 edge。因此
当前状态还包括 formal local publication 阻塞；不能把单节点或一次双节点 PASS 升级为
最终可复用交付。

## Direct local YOLO example

要向其他人演示构建物，使用固定 base SIF，并把独立 APP bundle 以只读方式挂载到
`/app`。完整的已实跑命令、SHA256、数值回执和清理证据见
[2026-09-10 checkpoint](../../../specs/183-tiger-yolo-reusable-experiments/evidence/tiger-runtime-checkpoint-20260910.md)。
最小入口如下（`RUN` 必须是新建的、不可复用的 run ID；维护入口按
`prepare` 后直接 `local`，不要再对同一 run 手动调用 `provision`）：

```bash
ROOT=/project/tma1/ndnsf-di/candidates/spec183-v49-20260910
RUN=minindn-local-<date>-<id>
OUT=/project/tma1/ndnsf-di/runs
export SPEC180_RUNTIME_SIF="$ROOT/planes/runtime/base-runtime-controller-version-j4-v23.sif"
export SPEC180_RUNTIME_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer
export SPEC180_RUNTIME_APP_ROOT="$ROOT/app-controller-version-j4-v39"
export PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper:$PWD/pythonWrapper"

# Freeze one new run; prepare intentionally exits 78/NOT_EVALUATED.
python3 Experiments/TigerCluster/jobs/yolo/submit.py prepare \
  --profile "$ROOT/profile-v49.json" \
  --run-id "$RUN" --output "$OUT" --case local-cpu || test $? -eq 78
python3 Experiments/TigerCluster/jobs/yolo/submit.py local \
  --profile "$ROOT/profile-v49.json" \
  --run-id "$RUN" --output "$OUT" --case local-cpu
```

`submit.py local` starts the registered four-provider MiniNDN graph; provider
and controller binaries come from the APP bundle and stable libraries come
from the SIF. Do not replace the APP with host binaries or inject host
libraries. This command demonstrates local CPU inference; it does not claim
Tiger GPU qualification.

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
