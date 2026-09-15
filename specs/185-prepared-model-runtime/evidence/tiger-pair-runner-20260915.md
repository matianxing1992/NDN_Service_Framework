# Tiger standard base SIF plus external APP runner repair

## Scope

本记录覆盖既有 `Experiments/TigerCluster` 配对运行器的本地门禁修正。沿用仓库已有
`build-local-sif.sh`、`build-sif-app.py`、`build-sif-app.sh` 和
`run-sif-app.sh`；没有重写 SIF 构建流程，也没有改变 Spec185 的 C++ 产品任务状态。

## Static gate

官方 `review-agent` 对冻结 immutable diff 返回 `STATIC_PASS`，无 P0--P3 控制性缺陷：

- base: `e89dd107`
- diff SHA-256: `40683221be943273130de20e428de1fce1bc61320629ea0577411b265e898fe4`
- scope: `run-sif-app.sh`、`test_sif_app.py`、`sif-app-delivery.md`

本轮修正并审查了：

- 单角色 `.ndn` 复制、单个 `*.privkey` 目录检查，以及私有副本 `tpmInfo` locator
  与 paired `NDN_CLIENT_PIB`/`NDN_CLIENT_TPM` 的一致性；
- NFD socket 的 owner/private/job scope/dedicated-directory 门禁、父目录 FD pin，
  以及容器内执行前的 socket type 和 device/inode 复核；
- `cleanenv`/`containall`、APP/base FD pin、非 shell `exec` 的退出状态传递和
  job-scoped scratch cleanup；
- 本地/Slurm 命令契约、测试断言和交付说明同步。

## Focused validation

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh  PASS
shellcheck -x Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh  PASS
pytest -q Experiments/TigerCluster/tests  81 passed, 1 skipped
git diff --check -- <three runner files>  PASS
```

这些是脚本和离线契约验证，不是产品资格验收。当前工作区没有可用的 regular base
SIF：`Experiments/TigerCluster/images/spec180-runtime-r119.sif` 仍是指向缺失目标的
broken symlink；此前 pair materialization 首个边界为
`APP_BASE_SIF_PATH_SYMLINK`。因此本轮没有运行真实 Apptainer/KeyChain/NFD、C++ 请求链、
SIF 构建、Slurm 或 TigerCluster，也没有上传任何制品。取得 regular base SIF 后，必须
按文档先在本机完成 pair materialization 和真实 C++ 请求验证，再做 Tiger 交付。
