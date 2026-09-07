# Reusable YOLO Distributed Inference

**Branch**: `TigerClusterExperiments`
**Status**: IN_PROGRESS / NOT_QUALIFIED

目标：一份profiles/yolo-two-node.json和一个jobs/yolo/submit.py入口，本地验证后，以同一完整SIF完成Tiger两节点四Provider推理并在新allocation复现。目前真实profile、SIF和worker allocation仍未取得，入口继续 fail-closed，不能执行规划中的submit命令。

## Current Checkpoint

Spec183 T001输入/接口清点完成，见[接收清单](../../../specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md)。三依赖需隔离接收锁定版本，NDNSD原目录有未提交改动不能覆盖；本地base SIF/当前签名YOLO package尚缺。远端base仅核对存在/大小。
现有builder host gate绑定Spec175 tiny-onnx，须增加明确的YOLO证据分支，不能用旧M01冒充。正常YOLO User是一次请求入口；warmup/measured分别执行和保存，不假定legacy sequential参数在当前路径有效。

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
