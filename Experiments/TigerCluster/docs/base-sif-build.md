# Stable base SIF refresh

## Validation And Permanent Retention

当前已验收版本：SHA-256 `8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9`，
4,087,824,384 bytes；见 [Spec187 封存证据](../../../specs/187-yolo-minindn-sif-app/evidence/b187-base-sdk-sealed.md)。

base 是后续构建的固定输入，必须先对最终 SIF（而不只是 builder/rootfs）执行
`base-runtime.py verify`、`dependency-sdk.py verify` 和真实 C++/ORT YOLO CPU smoke。
SDK 验收覆盖依赖摘要、C++ compile/link/run、ONNX checker/shape inference、NAC/SVS/NDNSD
符号、Rust、GStreamer、Python ABI/import、NumPy 私有库及 ELF 加载来源。
另以只读覆盖错误 SDK 库做拒绝反例，不能仅看成功路径。

通过后封存在仓库外 `/home/tianxing/NDN/ndnsf-artifacts/base-sif/<sha256>/`，
保存只读 `base.sif`、`SHA256SUMS`、依赖 manifest、构建输入/脚本、真实验收记录和续建来源。
该目录不入 Git，不属于 `.codex-tmp`、`images` 或构建缓存的自动清理范围；
删除须单独明确授权。封存后缺依赖或 ABI 变化生成新摘要、新目录，禁止原地修改。
跨机器传输后先核对 SHA-256，并在目标环境复验；本地 SDK/模型 PASS 不代表 MiniNDN/Tiger PASS。

入口由 TigerClusterExperiments 的 `build-base-libraries.sh` 改造而来。[两层交付](two-layer-delivery.md)要求 base 包含全部外部依赖；Core/DI/Repo/UAV 及绑定归 NDNSF 层。此前基础 smoke 镜像只覆盖 NDN-CXX/NFD、ORT、Python/NumPy，不能据此认定完整 SDK 已通过。

## Complete An Existing Base

在已有已验证 base 上增建，无需重做 NumPy/NDN/ORT 基础修复。`--dependency-bundle` 使用已封存 handoff 的外部源码与工具链，并要求其 base digest 与传入父镜像一致：

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-base-sif.py \
  --base /absolute/path/verified-base.sif \
  --dependency-bundle /absolute/path/verified-handoff \
  --output /absolute/path/new-sdk-base-run \
  --apptainer /usr/bin/apptainer --expected-apptainer 1.5.3
```

外部 NDN/crypto 依赖安装到 `/opt/ndn-base`，ONNX 到 `/opt/onnx`，Rust 到 `/opt/rust`；
NDN-SVS 的匹配 source/build pair 和 Cargo vendor 随 SDK 保留，供 NDNSF 消费而非重新下载。
新 `BASE_DEPENDENCY_SDK` 检查 headers/libraries/packages 身份、真实 C++/Rust compile/run 和动态闭包。
脚本已实现，实际新镜像验收状态以 Spec187 evidence 为准；历史 `BASE_SMOKE_ONLY` 不自动升级。
旧基础刷新入口和以下记录保留为初始基线流程，非完整 SDK 资格。

## Inputs

`../base-runtime.lock.json` 固定父镜像和 CPython 3.10 / x86_64 NumPy 1.26.4 wheel 的摘要。父镜像是已存在的依赖输入，本入口不是从操作系统开始的 bootstrap。更换父镜像或 ABI 时须更新锁定配置并重新验收，不能仅修改路径。

本机需 Apptainer **1.5.3**；父镜像下载完成后，构建分区至少保留 16 GiB。容器内安装 C++ 编译器和 Boost/OpenSSL 开发包，需能访问 Ubuntu APT 仓库；记录实际包版本，不声称 APT 离线复现。脚本和模板会复制进全新输出目录，保存其摘要；原始输入、定义、构建日志、最终镜像摘要和 smoke 结果留在该目录。失败目录保留，重试使用新目录。

## Build and smoke

从仓库根执行，`--output` 必须不存在，其父目录必须已存在：

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-base-sif.py \
  --base /absolute/path/parent.sif \
  --output /absolute/path/new-base-run \
  --apptainer /usr/bin/apptainer --expected-apptainer 1.5.3
```

可传 `--wheel /absolute/path/locked-numpy.whl` 离线复用；省略时下载 lock 中的 URL 并校验 SHA-256。输出包括 `base.sif`、`build-record.json`、`build.log` 和 `smoke.json`。不要把这些大制品或原始日志加入 Git。

NumPy 修复采用完整 wheel 重装，逐一比对 `numpy.libs` 中 OpenBLAS、gfortran、quadmath 三个文件的字节摘要，拒绝缺失、额外或变更文件。当前 base 是单阶段镜像，`%post` 修复后先检查，`%test` 和宿主随后启动最终 SIF 再检查；不会依赖只在 builder 中修复但未进入 final 的文件。

简单运行验证包括镜像内编译 C++ NDN-CXX/ORT consumer、运行它、NFD version、动态库闭包和 NumPy 矩阵乘法。结果范围是 `BASE_SMOKE_ONLY`，不能代替 APP 链接验证、模型推理、MiniNDN 或 TigerCluster 资格。

后续 NDNSF builder 必须以新 base 摘要重新绑定候选；不能沿用旧候选的 PASS。兼容 APP 布局仍由 `sif-app-delivery.md` 说明，不代表第三层。

初始 smoke base 的 OpenABE/RELIC 材料位于 `/opt/ndn-base/sdk/lib`。完整 SDK 增建将所需库明确安装并验证到 `/opt/ndn-base/lib`；NDNSF 不再使用未声明的 `/usr/local/lib`。NDNSF consumer 迁移与本地推理仍需独立验收。
