# Reusable YOLO Distributed Inference

## Current Deployment Decision — 2026-09-08

用户确认[基础SIF + 外置DI/UAV应用](runtime-app-layers.md)，设计ACCEPTED，
迁移IMPLEMENTATION_PENDING。以下旧“完整应用SIF/九产物”描述是已有实现记录；
以Spec183最新tasks/plan为执行入口。后续app-only改动只更新独立包，不重建
基础库SIF；最终验收绑定base+app+harness/model。C++小例子209981已在itiger01/02
实跑3条Data并清理成功，基础传输证据复用。历史 APP v35 + v22 base 已完成 exact-SIF
本机/host gate、Tiger 单节点 GPU `210340` 以及首个双节点正常 `210341`；v49 已在
同一分层边界完成正式 local、单节点 GPU `210365` 和双节点正常 `210366`。完整
可复用交付仍未资格化：负例 `210342` 暴露了 edge cardinality，`210373` 暴露了
rank 间冷准备造成的 completion budget 消耗，T015 需重跑且 T016 尚未运行。

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
**Status**: COMPLETE / V56_LOCAL_SINGLE_TWO_NODE_PASS / V56_NEGATIVE_EXPECTED_REJECTION_PASS / V56_REUSE_PASS

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
reuse T016. Single-node `210365` / `tiger-single-node-gpu-v49-r8` and normal
two-node `210366` / `tiger-two-node-gpu-v49-r13` now have complete Slurm,
CUDA/backend, numerical and cleanup receipts. Negative `210373` /
`tiger-negative-dependency-v49-r14` reached the exact native withheld edge and
Merge failure, but rank1's completion deadline expired while rank0 was still in
cold preparation; it has no User observation or collector verdict and does not
close T015. Full hashes and retained failures are in the [Spec183
checkpoint](../../../specs/183-tiger-yolo-reusable-experiments/evidence/tiger-runtime-checkpoint-20260910.md).

负例触发点已有源码和原生组件证据：`DetectShard0` 在真实V3输出校验后、
首包发布前阻止该请求到 `Merge` 的对象，并保留绑定的触发记录。见Spec183
`evidence/t004-dependency-cutpoint.md`。当前远端负例 `210342` 已真实走到
Selection/withheld，但因 User completion budget 和逻辑 edge cardinality 缺陷未
形成 collector 终态；v49 的 `210373` 已把逻辑 edge 收敛为一条，却暴露出
completion timer 在两 rank 到达时间不一致时过早开始：rank1 于 23:33:54
进入 barrier，rank0 到 23:35:12 才写入 User 生命周期。下一次 T015 必须把冷准备
时间纳入预算或延迟 arm barrier，不能手工移除保护或把部分日志标为 PASS。

目标：一份profiles/yolo-two-node.json和一个jobs/yolo/submit.py入口，本地验证后，以同一分层 base+APP 组合完成Tiger两节点四Provider推理并在新allocation复现。v49 已完成正式 local、单节点 GPU 和首次双节点正常 PASS；T015 需要一次有界负例重跑，T016 需要第二次不变配置的正常 allocation。

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
证据已在实际 allocation 使用。负例 `210342` 仍保留为历史失败；v49 负例
`210373` 已只有一条 bound DetectShard0→Merge edge，并且 Merge 写出精确 native
failure，但 rank1 的 completion budget 在 rank0 冷准备完成前耗尽，仍缺 User/
collection 终态。下一次 T015 必须把冷准备时间纳入 completion budget。新机器必须安装冻结 requirements-operator.txt 对应的操作者
依赖并保证batch解释器一致。Tiger已建立独立环境，当前profile的
runtime.operatorPython指向它；系统Python仍不作为该环境的替代。

历史 APP v35 已完成 host gate、exact-SIF MiniNDN Y-B/Y-N，以及
共享 `tiger-local-cpu-v35` 的 `NORMAL_EXPERIMENT_PASS`。它复用内容锁定的
v22 base SIF，只重建外置 APP 和受影响 planes。新的 v49 候选已在
`210365` 完成单节点 1+1，在 `210366` 的 `itiger02`/`itiger03` 完成双节点
1 warmup + 3 measured；四角色 backend、9 条依赖边/请求、数值与清理均闭合。
负例 `210342` 的两个同源逻辑 edge 问题已在 v49 收敛，`210373` 进一步证明
DetectShard0→Merge 的单 edge withholding 和 Merge native failure 可到达，但
completion timer 因 rank 间冷准备偏斜提前耗尽，仍没有 `negative-user.json`。
因此单节点和首次双节点门已关闭，T015 需一次预算修正后的重跑，T016 仍未运行；
不能把一次正常双节点 PASS 升级为最终可复用交付。

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

## 2026-09-11 qualification checkpoint (superseded historical v54/v55 section)

今天的 v54 分层组合（v23 base SIF + APP manifest
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`）
完成三条真实 PASS：local `tiger-local-cpu-v54-r1`、single-node GPU
`210402`/`itiger02` 和 two-node GPU `210403`/`itiger02,itiger03`。单节点为
1 warmup + 1 measured，三个模型角色 CUDA、Merge CPU；双节点为 1 warmup +
3 measured，四角色跨节点、9 条依赖边/请求、两 GPU、数值和清理闭合。对应
verdict SHA 分别为 `c6dc38bfd5b47dab418ce87ef5a231a985400d67b94ed5f7ba4a3f95aeec9e43`、
`7a07cbe3c62445f40e163dc7f8577a432ed2cecf56938e2be57a93ad7852655d` 和
`7b38c8328be2dc2e809428ca1db653744669745cf71d749f85d023a8204f35d9`。

collector 的 harness-only 修正处理真实 ndn-cxx `%NNI` 名称和 requestId
session 形式；focused regression 为 120 passed。由于 sealed harness 改变，v55
重新跑 local 和 single-node（`210440`/`itiger03`）并通过。v55 双节点
`210441`、`210455` 在 `itiger05,itiger06` 各自超过 900 秒，未形成
`collection-input.json`；`210458` 在同一节点对取消并记为 FAIL。不能把这些
超时当作 SIF/YOLO 正确性 PASS，也不能用 v54 two-node receipt 伪装 v55 gate。

历史 v54/v55 段落保留作失败边界；最终状态见下方 v56 闭环段落。

## v56 final operator checkpoint

The v56 run order is complete: local `tiger-local-cpu-v56-r1`, single-node
GPU Slurm `210471` on `itiger02`, first two-node normal `210472` on
`itiger02,itiger03`, negative dependency `210473`, and independent normal
reuse `210474`. Their authoritative verdicts and exact hashes are in the
[Spec183 closure](../../../specs/183-tiger-yolo-reusable-experiments/evidence/closure.md)
and [runtime checkpoint](../../../specs/183-tiger-yolo-reusable-experiments/evidence/tiger-runtime-checkpoint-20260911.md).

The unchanged base SIF is
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`
and the external read-only APP is
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`.
The v56 harness is `sha256:09d8bb4d237453acf1a7a33048363712c5bfdde87e8623a2be359f7eff969ed`.
Normal runs contain 1 warmup + 3 measured requests, four roles and nine
dependency edges per request; model roles use CUDA, Merge uses CPU, the
independent oracle matches shape `[1,50,6]`, and cleanup is closed. The
negative run is `EXPECTED_REJECTION_PASS` with one exact withheld edge and no
response or reselection. This is a correctness/reuse qualification, not a
performance claim.

真实参数、命令和成功示例见 [Spec183 evidence](../../../specs/183-tiger-yolo-reusable-experiments/evidence/minindn-v52-exact-sif-yb-v32.md)；未运行继续NOT_RUN，不复制历史PASS。计划见[Spec183](../../../specs/183-tiger-yolo-reusable-experiments/plan.md)。
