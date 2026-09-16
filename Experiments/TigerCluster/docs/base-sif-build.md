# Stable base SIF refresh

入口由 TigerClusterExperiments 的 `build-base-libraries.sh` 改造而来。当前 base 提供 NDN-CXX/NFD、ORT、Python/NumPy 和基础构建 SDK；Core/DI/SVS/NDNSD/NAC-ABE 与绑定在 APP 中构建。不会使用旧 `--runtime-libraries-only` 选项，也不会借用旧 DI 库来满足新绑定的链接要求。

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

此 base 的后续 APP builder 仍需以新 base 摘要重新绑定候选；不能沿用旧候选的 PASS。历史 SDK 兼容路径仅用于构建，生产 APP 仍采用 `sif-app-delivery.md` 的独立库集合和 RPATH。

OpenABE/RELIC 构建材料位于 `/opt/ndn-base/sdk/lib`，不会进入默认 loader 路径。使用它们的 APP recipe 必须显式选择 SDK 并把对应动态库装入 APP；旧 recipe 的 `/usr/local/lib/libopenabe.so` 等路径不能直接沿用。当前入口验收范围是基础镜像，不代表旧 APP recipe 已完成迁移。
