# Standard base SIF + NDNSF-DI APP delivery

本入口把部署对象固定为一对不可变制品：稳定的 `base.sif` 和由容器内已验证候选提取出的外置 `app/`。模型、密钥、实验配置和输出仍然是运行时挂载，不进入任一制品。APP 内的 C++ 对象只允许 `$ORIGIN/../lib` 与 `/opt/ndn-base/lib`，Python binding 只允许 `$ORIGIN/../../lib` 与 `/opt/ndn-base/lib`；构建和兼容 `current`/`stage`/源码路径由静态门及 APP packager 拒绝。

## Build locally

先在与 `base.sif` 同一 builder/依赖闭包内生成并验收一个应用候选 SIF。当前 `development-runtime.def.in` 会在容器内编译原生组件，并在最终镜像中发布无 symlink 的 `/opt/ndnsf-app`；APP 只放 DI 应用程序、`libndnsf-distributed-inference` 和绑定/配置，Core、SVS、NDNSD、NAC-ABE 等基础库只由 base 提供。候选必须有 `ndnsf-local-sif-build-v3` `PASS` 记录，且记录中的 `buildInput.baseSif.sha256` 必须等于要交付的 base SIF。只有旧 `/opt/ndnsf-di/current` 的 complete-application SIF 不能直接作为 pair 候选；脚本会在容器内布局检查处拒绝它。

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

上传前用同一运行器做本机验证。`--local` 只解除 Slurm 作业号要求，不改变 `cleanenv`、`containall`、APP 显式只读挂载、模型/制品只读挂载、证据可写挂载或应用路径优先级：

```bash
Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh \
  --local --base-sif /path/to/base.sif \
  --app /path/to/release/app \
  --app-record /path/to/release/app-manifest.json \
  --project /path/to/ndnsf-di \
  --scratch /tmp/ndnsf-di-local-pair \
  --identity /path/to/identity \
  --release-bind /path/to/releases \
  --models /path/to/models \
  --artifacts /path/to/artifacts \
  --evidence /path/to/evidence \
  --gpu-count 0 -- \
  /opt/ndnsf-di/app/bin/di-native-provider <provider-args>
```

本地验证必须记录 C++ 入口的真实启动、请求/ACK/Selection/Response、错误/取消和清理结果；仅 `--help`、Python import 或 `ldd` 不是协议资格。运行器结束后会删除本次 job-scoped scratch；证据应写入单独的 `--evidence` 挂载。失败保持原始日志和 `NOT_READY`/`UNQUALIFIED` 边界，不上传该 pair。

## Tiger execution

把同一 `base.sif`、`app/` 和 `app-manifest.json` 复制到项目存储后，在 Slurm 作业中调用同一 `run-sif-app.sh`（去掉 `--local`）。运行器重新计算 base 和 APP manifest，要求 `SLURM_JOB_ID`，并把 APP 挂载到 `/opt/ndnsf-di/app`。`PATH`、`LD_LIBRARY_PATH` 和 `PYTHONPATH` 只包含该 APP 及明确的 `/opt/ndn-base` 稳定层；base SIF 中可能残留的旧应用路径不会成为回退路径。APP 目录和 manifest 必须保持只读，输入路径不能是 symlink。

复合身份至少包含 base SIF、候选 SIF、容器构建记录、APP 文件清单、Apptainer 版本、模型/制品/profile 和实际挂载。当前发布的 pair manifest 使用 `ndnsf-sif-app-v2`，其中明确声明 `/opt/venv/bin/python`、`/opt/ndn-base/lib` 和 `/opt/onnxruntime/lib` 这组 base runtime contract；旧 v1 manifest 必须重新发布。任何一项改变都必须重新打包并在本地重跑；不能只替换 APP 文件或只修改 profile。

现有 `build-local-sif.sh` 与 `run-container.sh` 保留为完整应用 SIF 的兼容入口。新 pair 入口不自动提交 Slurm、构建 SIF 或上传 Tiger；只有本地验证通过后才进入人工交付步骤。
