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

### Self-contained Runtime Boundary

最终 SIF 内置 NDNSF 库、APP、bindings 及全部已声明的运行依赖；运行时不需要
宿主源码树、构建缓存、venv、库目录或相邻 base SIF。base 仅是构建输入。
模型、输入、部署配置、密钥和输出可以显式提供；不得暗中引用开发机固定目录。
构建后的 native probe 使用 `--cleanenv --containall --no-mount home,cwd,hostfs,bind-paths`，
并使用 disposable `--writable-tmpfs` 接收 verifier 的 manifest 写入，不修改原镜像。
单元测试挂载测试源码与输出、YOLO 挂载模型/输入与输出，不将这些测试输入当作生产运行库。
宿主仍需兼容 Linux/Apptainer；GPU 驱动属于另外声明的平台条件。

### Final SIF Packing Recovery

若完整两阶段构建已执行完 final post，仅 SIF 封装失败，可在 `--build-only`
调用中增加 `--resume-final-rootfs /absolute/path/retained-final-rootfs`。
入口仍检查原始两阶段 definition，并要求 rootfs 中保存的 definition 和 source
seal 与输入逐字节一致，再执行 SDK/native verifier 后封装。记录明确标注恢复方式；
该选项不能用于 release PASS，也不能代替最终镜像测试。磁盘预算必须同时包括
展开目录、临时压缩 squashfs 和最终 SIF；本机这一候选原两阶段构建应预留至少
32 GiB，恢复封装时仍需计入 sandbox 复制的临时占用。

### Local Native Unit Smoke

完整候选生成后，可先在容器内编译并运行既有 C++ DI 单元测试，再执行真实
YOLO 推理。测试源码使用指定 checkout 的 Git 提交，必须与镜像内 source seal
一致；编译器、生产头文件和生产库均来自候选。此检查不重编生产实现，不接受
宿主 `.so` 或测试二进制，也不代表 MiniNDN/Tiger 资格。

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-di-unit-smoke.py \
  --sif /absolute/path/candidate.sif \
  --expected-sif-sha256 <actual-sha256> \
  --source /absolute/path/source-checkout \
  --output /absolute/path/new-unit-run
```

范围为 Runtime、Preparation、NativePlan、YoloMerge 四组既有 C++ 测试。
输出保留 `run.log`、编译/执行命令、测试输入与二进制摘要以及 `record.json`；
只有实际执行成功才记录 `DI_CPP_UNIT_SMOKE_ONLY` 范围内的 PASS。

两层构建中 `NDNSF_LIBRARY_DIR` 仅指向包含 Core/DI 的
`/opt/ndnsf-stage/lib`。外部依赖由 pkg-config 和各自的 prefix 提供；
`/opt/ndn-base/lib` 保留在依赖搜索路径和运行时 RPATH 中，不能作为 Repo
binding 的显式 Core 库目录。模板和构建门禁共同检查这一约定。

### Historical Directory Migration Scope

此次目录整理只执行静态路径/内容核对和相关工具单测，不构建SIF、不提交Slurm、不跑MiniNDN。
将来交付需要按对应候选完成实际镜像闭包与运行验证；单纯目录移动不要求重跑无关C++测试。
