# Local SIF Build

构建入口：`Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh`。
构建前准备源码seal并核对definition、依赖和工具链，沿用 [runtime package](../../../packaging/ndnsf-di-container/README.md) 的原生ABI与候选规则。

当前三库源码固定、可迁移definition和接收机器步骤见 [source handoff](source-handoff.md)。
共享操作skill在仓库根 [skills/](../../../skills/README.md)；实际构建仍使用上述唯一入口。

## Output Layout

新构建显式选择新目录，例如：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --definition /absolute/path/to/sealed-runtime.def \
  --sif /absolute/path/to/Experiments/TigerCluster/images/<candidate>/runtime.sif \
  --record /absolute/path/to/Experiments/TigerCluster/images/<candidate>/build-record.json \
  --source-seal /absolute/path/to/source-seal.json \
  --host-gate-manifest /absolute/path/to/qualified-host-gate.json \
  --apptainer /absolute/path/to/qualified/apptainer \
  --expected-apptainer <qualified-compute-version>
```

这是需替换占位值的路径示例，本轮未构建；原脚本的`--help`打印用法并返回2，沿用既有行为。
SIF、缓存、私有身份、模型和大日志不入Git。镜像在容器builder内编译原生组件；宿主驱动构建，不提供宿主.so或venv作为运行依赖。

Spec183 YOLO 不得把旧 Spec175 清单冒充 host receipt。它使用显式 dispatch：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --workload-kind spec183-yolo \
  --definition /absolute/path/to/sealed-runtime.def \
  --sif /absolute/path/to/Experiments/TigerCluster/images/<candidate>/runtime.sif \
  --record /absolute/path/to/Experiments/TigerCluster/images/<candidate>/build-record.json \
  --source-seal /absolute/path/to/source-seal.json \
  --spec183-host-gate /absolute/path/to/tiger-yolo-host-minindn-manifest.json \
  --apptainer /absolute/path/to/qualified/apptainer \
  --expected-apptainer <qualified-compute-version>
```

该 receipt 必须由真实 CPU/MiniNDN qualification 产生，并绑定同一 source
seal、`/<applicationName>/sync` 应用组、四 Provider 及三类注册 case；当前
validator 只提供 `YOLO_HOST_GATE_COMPONENT_ONLY` 边界，不替代 T010 的真实
receipt。Spec183 receipt 会在 Apptainer version/build 调用前校验；无效或混用
旧参数必须零 Apptainer 调用。旧 Spec175 路径和其命令顺序保持不变。

## Existing Images

本机较新历史候选为 `.local-tmp/spec180-candidate-r119/spec180-runtime.sif`；
同目录有 `spec180-runtime-final.def`、`build.log`和run record。
`images/spec180-runtime-r119.sif`仅为指向该文件的本地链接，不复制或修改约3.28 GiB镜像。
其已记录的集群路径为 `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`，
本轮未登录集群确认。候选文件存在不代表当前源码或GPU验收通过。

## Validation

此次目录整理只执行静态路径/内容核对和相关工具单测，不构建SIF、不提交Slurm、不跑MiniNDN。
将来交付需要按对应候选完成实际镜像闭包与运行验证；单纯目录移动不要求重跑无关C++测试。
