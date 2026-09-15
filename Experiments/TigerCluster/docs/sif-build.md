# Local SIF Build

## Current Delivery Direction

2026-09-08 已接受后续使用**稳定基础 SIF + 外置版本化 DI/UAV 应用包**。
详细归属、builder ABI、只读挂载、组合身份与验收见
[Layered Runtime Delivery](../../../specs/182-native-di-python-bindings/contracts/layered-runtime-delivery.md)。
应用更新可复用未变基础 SIF，但须在对应 builder 内构建并验证新组合。
目前为 ACCEPTED DESIGN / PLANNED TOOLING；下面的 complete-SIF 命令是兼容入口，
不能仅加一个 bind 就声称已支持分层发布。默认先在本地实验 host 用 Apptainer
1.5.3 构建并检查稳定基础 SIF，再把同一 hash 的 SIF 上传到项目存储；Tiger
计算节点只做同一镜像的 inspect、preflight 和 GPU/MiniNDN 运行验证。若本地
helper、文件系统或权限阻断 materialization/build，必须保留首失败并以新的
source-sealed candidate 在 compute 1.5.3 例外构建；登录节点 1.3.4 不得参与。

## Existing Build Entry

构建入口：`Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh`。
构建前准备源码seal并核对definition、依赖和工具链，沿用 [runtime package](../../../packaging/ndnsf-di-container/README.md) 的原生ABI与候选规则。

在调用构建入口前，先运行同目录的
`preflight-development-sif.py`。它用秒级检查交叉核对 rendered definition
的输入文件、sealed `workspace.tar` 中显式 Waf target 的模板、NumPy
wheel-private DSOs/RPATH，以及基础 SIF 中的 NumPy 导入；`build-local-sif.sh`
也会在解析 `localimage` base 后自动重复这个门。它还扫描 builder shell 中
实际被 `cp`/pip 消费的 `/src/ndnsf/...` 路径，确认这些子目录真的封存在
`workspace.tar`，并在提供 base SIF 时检查 definition 要求的编译器、ONNX SDK、
Rust 和系统头文件能力。该门失败时禁止开始完整原生编译，应根据
`docs/failure-log.md` 的对应条目修复输入后再建立新的候选。

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-development-sif.py \
  --definition /absolute/path/to/rendered-runtime.def \
  --apptainer /usr/local/bin/apptainer \
  --base-sif /absolute/path/to/base.sif
```

如果 SIF 已经完成构建、只是 `import`、loader 或后续运行门失败，不要重新编译：
使用 `build-local-sif.sh --verify-existing` 复用同一个 SIF。该模式仍检查源码
seal、definition labels、base 与 Apptainer 版本、SIF hash 和完整运行时 preflight，
但记录方法为 `local-apptainer-existing-sif-verify`。输出 SIF 或记录已经存在时，
普通构建模式仍然 fail-closed，避免覆盖可追溯候选。

当前三库源码固定、可迁移definition和接收机器步骤见 [source handoff](source-handoff.md)。
共享操作skill在仓库根 [skills/](../../../skills/README.md)；实际构建仍使用上述唯一入口。

## 先复用成功模板，再开始构建

Spec186 的每个新候选都必须先阅读
[successful Tiger GPU template](successful-tiger-gpu-template.md)。它保存了最近
一次完整 YOLO GPU 候选的可复用形状：base SIF SHA
`44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`、Clang 10
和 `-O0 -g0 -B/usr/bin`、Apptainer 1.5.3、只读 app、固定模型 oracle 以及
single/two-node 的终态字段。它不是当前候选的运行证据；source、base、app、
compiler、profile、model 或 harness 任一变化都要产生新的 candidate。

不要从上一次失败的 rendered definition 继续编辑。先把模板与新候选的
source seal、base SIF SHA/bytes、definition、compiler/toolchain root、Waf 的
NDN-SVS source/build pair、pkg-config、final-stage transfer 和运行 profile
列成差异表，再由 `prepare-development-handoff.py render` 生成 definition。
渲染后依次执行 shell/Python/边界/preflight 检查，确认 `--toolchain-root`、
Clang 路径和 `CXXFLAGS` 与模板一致，之后才调用 `build-local-sif.sh`。任何
不在差异表中的改动都按失败处理并写入 failure log；不能通过删除 apt、换
GCC、换 intermediate base 或减少目标来“试一下”。

这条顺序对应 Apptainer 官方定义文件规则：`localimage` 可在构建时验证
SIF base，multi-stage `%files from` 只从先前 stage 复制，定义文件本身应作为
构建元数据保存；参见 [Definition Files](https://apptainer.org/user-docs/master/definition_files.html)
和 [Build a Container](https://apptainer.org/docs/user/main/build_a_container.html)。
若要声明可复现输出，应固定 `SOURCE_DATE_EPOCH`，并避免会使定义元数据失真的
`--section` 用法。

## Output Layout

## Tiger 节点版本边界

| 节点 | Apptainer | 允许用途 | SIF 构建/检查/执行 |
| --- | --- | --- | --- |
| 登录节点 | `/usr/bin/apptainer` 1.3.4 | SSH、Slurm 提交和路径/资源元数据 | 禁止；不执行任何 SIF 操作 |
| 计算节点 | `/home/tma1/.local/bin/apptainer-1.5.3` 1.5.3 | 分配到的节点上的构建、检查、preflight 和运行 | 唯一允许的 Tiger CLI |

因此本项目不需要用登录节点的 1.3.4 构建 SIF；SIF 只用计算节点的
Apptainer 1.5.3 构建，并在同一 1.5.3 运行时完成 inspect、preflight 和执行。
登录节点只负责把作业送入 Slurm，不能作为版本回退或隐式 builder。默认 SIF 在
本地实验 host 的 1.5.3 构建，compute 1.5.3 核对同一 SHA 后执行；只有本地路径
有已记录阻断时，才允许 compute 1.5.3 例外构建，并在新 candidate 中记录替代关系。

## Recommended Build And Upload Flow

1. 在本地实验 host 用 `/usr/local/bin/apptainer` 1.5.3 构建 source-sealed
   基础 SIF，并运行 `preflight-development-sif.py`、`import`、入口
   `--help`、`readelf`/`ldd -r` 和本地 MiniNDN CPU smoke。
2. 记录 SIF 字节数、SHA-256、source seal、definition、依赖和 app bundle
   digest；任何一项改变都生成新的候选，不覆盖旧镜像。
3. 将 SIF 上传到 Tiger 项目存储后，在登录节点只做路径/配额/Slurm 元数据
   检查；不得调用登录节点 1.3.4 的 build、inspect 或 exec。
4. 在分配到的计算节点用
   `/home/tma1/.local/bin/apptainer-1.5.3` 校验同一 SHA-256，随后执行
   `--nv` 的 bounded GPU/MiniNDN 运行。Tiger 的 CUDA、NFD、Slurm、节点
   映射和跨进程协议证据仍必须单独收集；本地 smoke 不能替代这些证据。

这样可以把原生编译和 SIF 封装从 Tiger 作业中移出；应用只读 bundle 的变更
不重建未变化的基础 SIF。当前完整镜像约 4 GiB，上传前必须检查项目配额，
并只保留按 digest 命名的候选，避免重复占用存储。

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

复用已生成候选的命令只需增加 `--verify-existing`，其他输入必须保持相同：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh \
  --verify-existing \
  --definition /absolute/path/to/sealed-runtime.def \
  --sif /absolute/path/to/Experiments/TigerCluster/images/<candidate>/runtime.sif \
  --record /absolute/path/to/Experiments/TigerCluster/images/<candidate>/verify-record.json \
  --source-seal /absolute/path/to/source-seal.json \
  --host-gate-manifest /absolute/path/to/qualified-host-gate.json \
  --apptainer /usr/local/bin/apptainer \
  --expected-apptainer 1.5.3
```

这是需替换占位值的路径示例。本机实验 host 的唯一 Apptainer 是
`/usr/local/bin/apptainer` 1.5.3；Tiger login node 的 1.3.4 不参与 SIF
构建或执行。Tiger compute 使用项目目录中的
`/home/tma1/.local/bin/apptainer-1.5.3`（1.5.3）；登录节点
`/usr/bin/apptainer` 的 1.3.4 只用于 SSH/Slurm 元数据，不能作为构建或运行
回退。原脚本的`--help`打印用法并返回2，沿用既有行为。
SIF、缓存、私有身份、模型和大日志不入Git。镜像在容器builder内编译原生组件；宿主驱动构建，不提供宿主.so或venv作为运行依赖。

Tiger 账户没有 subordinate UID/GID，且 v23 Ubuntu 20.04 基础镜像的 glibc
无法运行新 fakeroot helper。计算节点构建因此显式设置
`SPEC186_APPTAINER_CONFIG=/etc/apptainer/apptainer.conf` 和
`SPEC186_APPTAINER_ROOT_MAPPED=1`；入口会以同一个 1.5.3 CLI 加上
`--ignore-subuid --ignore-fakeroot-command` 建立 root-mapped builder。这个
模式只解决构建权限边界，最终 SIF 仍由 1.5.3 构建、检查并在计算节点执行。
登录节点 1.3.4 不得参与任何一个步骤。

## Existing Images

本机较新历史候选为 `.local-tmp/spec180-candidate-r119/spec180-runtime.sif`；
同目录有 `spec180-runtime-final.def`、`build.log`和run record。
`images/spec180-runtime-r119.sif`仅为指向该文件的本地链接，不复制或修改约3.28 GiB镜像。
其已记录的集群路径为 `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`，
本轮未登录集群确认。候选文件存在不代表当前源码或GPU验收通过。

## Validation

此次目录整理只执行静态路径/内容核对和相关工具单测，不构建SIF、不提交Slurm、不跑MiniNDN。
将来交付需要按对应候选完成实际镜像闭包与运行验证；单纯目录移动不要求重跑无关C++测试。
