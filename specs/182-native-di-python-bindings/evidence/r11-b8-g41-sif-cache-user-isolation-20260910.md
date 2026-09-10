# R11-B8-G41 SIF Cache User Isolation

日期：2026-09-10

## Boundary

`run-container.sh` 原来把 immutable SIF cache 放在共享的
`/tmp/ndnsf-di-sif-cache/<digest>`，并在该目录直接执行 `exec 9>stage.lock`。
在多用户计算节点上，其他用户可以预置同名 symlink，使 lock 打开阶段重定向到
非本作业文件；MiniNDN 单用户运行不会暴露该边界。

## Correction

cache 现在使用 `<cache-root>/u<uid>/<digest>`，并在打开 lock 前将 uid 目录和 digest
目录设为 `0700`。同一用户的不同 job 仍按 digest 复用 node-local SIF；跨用户不能写入
或替换锁文件。SIF 的 build-record、完整 SHA-256 和 Apptainer 执行门保持不变。

## Verification

```text
python3 -m pytest -q tests/python/test_spec170_sif_build_record.py tests/container/unit/test_slurm_node_scripts.py
16 passed
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-container.sh
PASS
git diff --check
PASS
```

本批只验证 cache 隔离和现有 runner gate；没有把 node-local staging 记作真实 SIF 或多机
资格 PASS。
