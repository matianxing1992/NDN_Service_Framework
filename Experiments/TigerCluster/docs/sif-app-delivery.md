# Standard base SIF + NDNSF-DI APP delivery

逻辑构建采用 **base SIF + NDNSF**：base 提供全部已声明外部依赖及 SDK，NDNSF 层拥有本仓库框架、DI、Repo、UAV、绑定和应用。旧 `app/` 是 NDNSF 层的兼容打包名称；不能把只有 base 的模型冒烟当成最终组合验收。具体归属与增建规则见 [two-layer-delivery.md](two-layer-delivery.md)。

本入口把部署对象固定为一对不可变制品：稳定的 `base.sif` 和由容器内已验证候选提取出的外置 `app/`。模型、密钥、实验配置和输出仍然是运行时挂载，不进入任一制品。APP 内的 C++ 对象只允许 `$ORIGIN/../lib` 与 `/opt/ndn-base/lib`，Python binding 只允许 `$ORIGIN/../../lib` 与 `/opt/ndn-base/lib`；构建和兼容 `current`/`stage`/源码路径由静态门及 APP packager 拒绝。

补齐 SDK 后，外部库、头文件和 pkg-config 统一来自 `/opt/ndn-base`，ONNX 与 Rust 分别位于 `/opt/onnx`、`/opt/rust`。NDNSF builder 为兼容旧 APP 校验复制必要的 base 库，不重新编译它们；`/opt/ndn-base/sdk/lib` 不加入运行时 loader 路径。CPython 头文件以容器解释器的 `sysconfig` 为准，不从宿主装入。历史 [B187-APP-SDK](../../../specs/187-yolo-minindn-sif-app/evidence/b187-app-sdk.md) 仅证明当时适配；当前完整候选验收仍为 PARTIAL。

## Build locally

开发中的当前源码可先用同一入口增加 `--build-only`，省略 `--host-gate-manifest` 与 `--strict-host-source-seal`，得到 `BUILT_UNQUALIFIED` 本机测试候选。它保留源码/容器边界检查和模板内原生验收，但不声称 Spec175 资格。`validate-local-sif-build-record.py` 和正式 APP packager 会拒绝该状态；不能改 JSON 为 PASS 来发布。完整发布仍使用下述带 host gate 的流程。

当前 handoff 要求 `--native-inputs`：由 `prepare-native-build-inputs.py` 封存官方 ONNX 1.17 源码、Rust 1.90 安装器和 Cargo.lock 对应的离线 vendor 源码；六个文件摘要写入 lock 的 `nativeBuild.files`。base 增建消费外部输入；NDNSF 仅核对相同身份并编译本仓库 tokenizer bridge，禁止复制宿主 `.a`。`native-inputs.json` 摘要绑定 definition 和 base SDK manifest。

在同一已验证 base SDK 内构建 NDNSF 候选 SIF。`development-runtime.def.in` 仅编译仓库组件，最终同时发布完整 SIF 的兼容路径与无 symlink 的 `/opt/ndnsf-app`。后者保留 Core/DI、绑定与所需 base 库的逐字节副本；外部依赖归 base。正式 pair 发布仍要求 `ndnsf-local-sif-build-v3` `PASS` 记录，且 `buildInput.baseSif.sha256` 等于交付 base 的摘要；`BUILT_UNQUALIFIED` 只用于本机诊断，不满足发布要求。

然后只需一次 pair 打包：

```bash
mkdir -p /path/to/release
Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-sif-app.sh \
  --base-sif /path/to/base.sif \
  --candidate-sif /path/to/candidate.sif \
  --candidate-record /path/to/build-record.json \
  --output /path/to/release/app \
  --record /path/to/release/app-manifest.json \
  --apptainer /usr/local/bin/apptainer \
  --expected-apptainer 1.5.3
```

该命令会重新核对 base/candidate 的 SHA-256、构建记录、Apptainer 版本和 `/opt/ndnsf-app` 的完整 ELF/Python 闭包，并先在 base SIF 内确认 `/opt/venv/bin/python`、`/opt/ndn-base/lib` 和 `/opt/onnxruntime/lib` 存在。随后通过 `apptainer exec --cleanenv --containall` 将候选的完整应用树复制到外置 APP，再对绑定后的准确树重跑文件类型、ELF 依赖和 Python import 检查。`--apptainer` 可以指向系统 launcher symlink，但记录和执行会固定其解析后的 regular binary 及 SHA-256。宿主工作区中的 `.so`、Python extension、venv 或 build 目录不会被读取。输出目录和 manifest 已发布后均不可覆盖；manifest 的 `appDigest` 绑定所有文件、base、candidate、构建记录和挂载表。发布使用确定性的进程锁、候选 SIF 独立快照和 Apptainer 文件描述符；APP 与 manifest 之间用可恢复事务标记协调，崩溃后下一次调用会清理未完成发布或收束已完成 pair。

## Local pre-Tiger verification

最小 CPU 模型检查可先执行下列入口。它在指定 SIF 内编译 C++ ORT consumer，对锁定 YOLO26n graph/weights、固定 P3 输入和独立 PyTorch oracle 比较三次推理，并保留源/ELF/镜像摘要与硬超时记录。只产生 `YOLO_CPU_MODEL_SMOKE_ONLY`，不替代下面的 NDNSF 请求链检查：

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-yolo-cpu-smoke.py \
  --base-sif /path/to/candidate.sif --expected-sif-sha256 <sha256> \
  --model-root /path/to/spec180-yolo-candidate-current \
  --output /path/to/new-run
```

本机 base 运行结果见 [YOLO CPU evidence](../../../specs/187-yolo-minindn-sif-app/evidence/yolo-base-cpu-20260915.md)；固定小图仅验证回归一致性，不是照片识别准确率或性能基准。

上传前用同一运行器做本机验证。`--local` 只解除 Slurm 作业号要求，不改变 `cleanenv`、`containall`、APP 显式只读挂载、模型/制品只读挂载、证据可写挂载或应用路径优先级：

```bash
Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh \
  --local --base-sif /path/to/base.sif \
  --app /path/to/release/app \
  --app-record /path/to/release/app-manifest.json \
  --project /path/to/ndnsf-di \
  --scratch /tmp/ndnsf-di-local-pair \
  --identity /path/to/identity \
  --nfd-socket /tmp/ndnsf-di-local-nfd/nfd.sock \
  --release-bind /path/to/releases \
  --models /path/to/models \
  --artifacts /path/to/artifacts \
  --evidence /path/to/evidence \
  --gpu-count 0 -- \
  /opt/ndnsf-di/app/bin/di-native-provider <provider-args>
```

`--identity` 必须是单一运行角色的 home，包含无符号链接的
`.ndn/pib.db` 与作为目录的 `.ndn/ndnsec-key-file`（其中恰有一个 regular
`*.privkey`）。运行器会把它复制到本次私有 scratch 的 `$HOME/.ndn`，同步重写
副本 `tpmInfo` 的 locator，并设置成对的 `NDN_CLIENT_PIB`/`NDN_CLIENT_TPM`；不会直接打开
共享身份源。`--nfd-socket` 必须指向本机或当前计算节点上已启动的 NFD Unix
socket 文件；运行器将该文件的父目录只读绑定到容器 `/tmp/ndnsf-di-nfd`，保留 socket
文件名并设置
`NDN_CLIENT_TRANSPORT`。socket 的父目录必须由当前用户拥有、权限仅限 owner、位于
`/tmp/ndnsf-di-<job-id>/`（本地模式为 `/tmp/ndnsf-di-local-*`），且除该 socket 外不得有
其他条目；这样整个只读绑定只暴露一个专用 endpoint。运行器只负责连接既有 NFD 和执行一个
APP 命令，不代替 NFD 的启动、配置或生命周期管理。

本地验证必须记录 C++ 入口的真实启动、请求/ACK/Selection/Response、错误/取消和清理结果；仅 `--help`、Python import 或 `ldd` 不是协议资格。运行器结束后会删除本次 job-scoped scratch；证据应写入单独的 `--evidence` 挂载。失败保持原始日志和 `NOT_READY`/`UNQUALIFIED` 边界，不上传该 pair。

## Tiger execution

把同一 `base.sif`、`app/` 和 `app-manifest.json` 复制到项目存储后，在 Slurm 作业中调用同一 `run-sif-app.sh`（去掉 `--local`）。运行器重新计算 base 和 APP manifest，要求 `SLURM_JOB_ID`，并把 APP 挂载到 `/opt/ndnsf-di/app`。`PATH`、`LD_LIBRARY_PATH` 和 `PYTHONPATH` 只包含该 APP 及明确的 `/opt/ndn-base` 稳定层；base SIF 中可能残留的旧应用路径不会成为回退路径。APP 目录和 manifest 必须保持只读，输入路径不能是 symlink。

复合身份至少包含 base SIF、候选 SIF、容器构建记录、APP 文件清单、Apptainer 版本、模型/制品/profile 和实际挂载。当前发布的 pair manifest 使用 `ndnsf-sif-app-v2`，其中明确声明 APP native library allowlist 以及 `/opt/venv/bin/python`、`/opt/ndn-base/lib` 和 `/opt/onnxruntime/lib` 这组 base runtime contract；旧 v1 manifest 必须重新发布。任何一项改变都必须重新打包并在本地重跑；不能只替换 APP 文件或只修改 profile。

现有 `build-local-sif.sh` 与 `run-container.sh` 保留为完整应用 SIF 的兼容入口。新 pair 入口不自动提交 Slurm、构建 SIF 或上传 Tiger；只有本地验证通过后才进入人工交付步骤。
