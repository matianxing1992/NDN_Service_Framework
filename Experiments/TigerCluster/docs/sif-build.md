# Local SIF Build

稳定依赖 base 的独立构建入口见 [Stable base SIF refresh](base-sif-build.md)。它修复锁定 NumPy wheel 的私有库并执行最终镜像基础冒烟检查；完整 APP 仍使用下述现有入口。

## Apptainer Version Policy

2026-09-12 起，所有机器上的后续 NDNSF SIF 构建统一使用 Apptainer **1.5.3**，包括基础SIF、完整SIF及调用Apptainer的分层应用builder。开工前核对实际可执行文件、版本和SHA-256；不是1.5.3时先升级，禁止为通过门禁把预期版本改为旧版本。未来变更版本须明确修订本规则。
用户确认Tiger计算节点为1.5.3、登录节点为1.3；登录节点只负责传输/提交时，其版本不作为构建目标。
新构建传入 `--apptainer /usr/bin/apptainer --expected-apptainer 1.5.3`；另一台实验机需先核对自身安装与计算作业内版本。
已有脚本的版本一致性检查保留，不改成忽略版本。历史1.3.4记录和已冻结候选保持原事实。
若工作流确实在登录节点执行容器，该步骤仍需迁到计算节点或单独解决兼容，不因本机升级自动获得兼容性。
本机升级与有限验证见[记录](apptainer-153-upgrade-20260912.md)；不代表现有NDNSF候选或Tiger实验重新通过资格。

## Current Delivery Direction

2026-09-15 用户将当前方向简化为 **base SIF + NDNSF**；全部外部依赖与 SDK 归 base，本仓库模块统一归 NDNSF。优先在已有 base 上补齐依赖。详见 [two-layer delivery](two-layer-delivery.md) 与 [base completion](base-sif-build.md)。以下旧 APP 名称保留兼容和历史说明，不额外划出应用构建层；当前迁移未验收部分保持 PARTIAL。

2026-09-08 已接受后续使用**稳定基础 SIF + 外置版本化 DI/UAV 应用包**。
详细归属、builder ABI、只读挂载、组合身份与验收见
[Layered Runtime Delivery](../../../specs/182-native-di-python-bindings/contracts/layered-runtime-delivery.md)。
应用更新可复用未变基础 SIF，但须在对应 builder 内构建并验证新组合。
pair 入口见 [SIF + APP delivery](sif-app-delivery.md)。应用候选必须采用
`/opt/ndnsf-app` 分层布局。当前 `development-runtime.def.in` 在同一
container-native builder 中保留旧 `/opt/ndnsf-di/current` 兼容检查树，同时发布
无 symlink 的 `/opt/ndnsf-app` 候选树（包含 DI 应用程序及其 Core、SVS、NDNSD、NAC-ABE、
OpenABE、Relic 和 DI 原生库闭包，稳定 NDN-CXX/NFD/ONNX Runtime/Python 由 base 提供）；旧
`build-local-sif.sh` 生成且没有该候选树的
`/opt/ndnsf-di/current` complete-application SIF 仍可回放历史候选，但不能直接
作为新的 pair APP 来源。pair 入口只接受容器内验证的候选，不接受宿主编译的
`.so`、Python extension 或 venv。

## Existing Build Entry

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
  --expected-apptainer 1.5.3
```

这是需替换占位值的路径示例，本轮未构建；原脚本的`--help`打印用法并返回2，沿用既有行为。
SIF、缓存、私有身份、模型和大日志不入Git。镜像在容器builder内编译原生组件；宿主驱动构建，不提供宿主.so或venv作为运行依赖。

## Existing Images

本机较新历史候选为 `.local-tmp/spec180-candidate-r119/spec180-runtime.sif`；
同目录有 `spec180-runtime-final.def`、`build.log`和run record。
`images/spec180-runtime-r119.sif`仅为指向该文件的本地链接，不复制或修改约3.28 GiB镜像。
其已记录的集群路径为 `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`，
本轮未登录集群确认。候选文件存在不代表当前源码或GPU验收通过。

## Validation

此次目录整理只执行静态路径/内容核对和相关工具单测，不构建SIF、不提交Slurm、不跑MiniNDN。
将来交付需要按对应候选完成实际镜像闭包与运行验证；单纯目录移动不要求重跑无关C++测试。
