# Reusable YOLO Distributed Inference

**Branch**: `TigerClusterExperiments`
**Status**: IN_PROGRESS / NOT_QUALIFIED

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
旧六产物镜像在启动前拒绝。实际 T010 receipt/新 SIF 尚未产生，不能把组件用例
当成真实 SIF 内身份签发及请求验证。正常单/双GPU run、节点scratch、外部
collect --reconcile 和共享目录接收端submit已接；跨机器文件运输、可移植前置
证据和负例仍缺。新机器必须安装冻结 requirements-operator.txt 对应的操作者
依赖并保证batch解释器一致。Tiger已建立独立环境，当前profile的
runtime.operatorPython指向它；系统Python仍不作为该环境的替代。

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
profile选择同一解释器。本地CPU和普通离线collect使用调用者本机解释器，进入
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
- 最大-j2；SIF本地构建、Tiger只验证/运行；CAS缓存不按run复制模型，容量按实际峰值检查。

真实参数、命令和成功示例在验收后补充；未运行继续NOT_RUN，不复制历史PASS。计划见[Spec183](../../../specs/183-tiger-yolo-reusable-experiments/plan.md)。
