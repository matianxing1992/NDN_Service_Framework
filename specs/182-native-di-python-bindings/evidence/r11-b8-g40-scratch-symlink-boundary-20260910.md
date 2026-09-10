# R11-B8-G40 Scratch Symlink Boundary

日期：2026-09-10

## Boundary

静态复核发现 scratch 的 job basename 校验不能识别符号链接。一个名为
`/tmp/ndnsf-di-<SLURM_JOB_ID>` 的链接可以把 preflight 证据、topology socket、进程
HOME 或清理路径重定向到另一目录；canonical runner 随后解析出不同根目录，MiniNDN
和普通目录测试都不会覆盖该分歧。

## Correction

- `preflight-compute.sh` 在创建 evidence 前使用 `readlink -f`，要求输入路径与实际
  目录一致。
- `run-container.sh` 同时拒绝 `..` 组件、直接或父路径 symlink，并保留 job-bound
  basename 校验。
- `run-allocation-topology.sh` 在创建 topology state 前执行同一解析检查。
- `allocation_topology.render_process_launcher` 为直接调用方增加同一 scratch symlink
  门，避免绕过 supervisor。

这些检查只保护 scratch 根目录；workdir/identity 内容 digest、跨作业端口租约、exact-SIF
canonical runner 及真实 Slurm/SIF/跨节点 NDN 资格仍保持开放。

## Verification

```text
python3 -m pytest -q tests/container/unit/test_slurm_node_scripts.py tests/container/itiger-qwen-live/unit/test_allocation_topology.py tests/python/test_spec170_sif_build_record.py
42 passed
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS (fresh normal rerun; the first transient fixture retry is indexed in `docs/failure-log.md`)
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-compute.sh Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-container.sh Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
PASS
git diff --check
PASS
```

静态审查覆盖完整 diff、调用方、错误分类和 test-mode 边界；没有把该 pre-start 门写成
多机资格 PASS。
