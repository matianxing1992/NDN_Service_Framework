# Local SIF Build

## Accepted Layered Target — 2026-09-08

用户要求采用[基础库SIF + 外部应用](runtime-app-layers.md)。SIF包含NDN/NDNSF/
Repo通用库及运行依赖；DI/UAV程序、包及自有扩展独立构建、冻结和只读挂载。
应用变化仅增量编译受影响目标，基础ABI/依赖变化才重建SIF及消费者。
此决策已进入Spec183 FR-005/006与T002/T004/T011；**IMPLEMENTATION_PENDING**。
以下builder/完整应用镜像命令描述迁移前现状，尚不是分层构建命令。
实现时沿用这些owner拆分输出与预检，禁止临时注入宿主基础库；最终检查针对
精确base+app组合。已有合格base缓存可复用，不因每次app发布而重新打包。

仓库根 `wscript` 现在提供分层构建边界：基础层使用
`--runtime-libraries-only`（NDNSF Core、Repo 通用静态库和绑定），外置层使用
`--external-application-only --application-component=<di|uav|repo|all>`。YOLO
入口固定选择 `di`；TigerCluster 构建并行度上限为 `-j4`，基础层默认使用
`-j2` 以避免 Ubuntu 20.04 GCC 9 的资源敏感 ICE，外置应用可在 1–4 之间显式选择。
同一构建树不可同时运行两个 Waf/CMake 构建。外置模式通过 base 安装的
pkg-config/库闭包，不把 Core 或 Repo 源码重新编进 DI 应用。

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
seal、`applicationName + '/sync'`（applicationName 自带前导 `/`）应用组、四
Provider 及三类注册 case；当前
validator 只提供 `YOLO_HOST_GATE_COMPONENT_ONLY` 边界，不替代 T010 的真实
receipt。Spec183 receipt 会在 Apptainer version/build 调用前校验；无效或混用
旧参数必须零 Apptainer 调用。入口还要求真实的
`Experiments/TigerCluster/bin/ndnsf-di-spec183-preflight`：它必须先验证
sealed inputs，再验证最终 SIF 的 Python/native import、DSO/`ldd` 和实际
entrypoints。preflight 脚本已交付，但真实 source seal、完整 harness、SIF
及 T011 的运行证据仍未齐全；任何缺口都会让 Spec183 构建 fail closed。
旧 Spec175 路径和其命令顺序保持不变。

## Existing Images

本机较新历史候选为 `.local-tmp/spec180-candidate-r119/spec180-runtime.sif`；
同目录有 `spec180-runtime-final.def`、`build.log`和run record。
`images/spec180-runtime-r119.sif`仅为指向该文件的本地链接，不复制或修改约3.28 GiB镜像。
其已记录的集群路径为 `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`，
本轮未登录集群确认。候选文件存在不代表当前源码或GPU验收通过。

## Validation

此次目录整理只执行静态路径/内容核对和相关工具单测，不构建SIF、不提交Slurm、不跑MiniNDN。
将来交付需要按对应候选完成实际镜像闭包与运行验证；单纯目录移动不要求重跑无关C++测试。
