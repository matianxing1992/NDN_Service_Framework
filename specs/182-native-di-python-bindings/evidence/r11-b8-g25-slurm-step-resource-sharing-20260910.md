# R11-B8-G25 Slurm Step Resource Sharing

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION`（仅限多机启动器资源调度边界）
**Baseline**: `b8164f68`
**Review trace**: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；审查完整
未提交差异及相邻调用方，范围为三份 Slurm 脚本、network integration、Spec110/
Spec182 contract、audit 和 tasks。

## Finding and repair

代码审查发现 topology supervisor、route configurator 和 network probe 的每个
`srun` 都带有 `--exclusive`。同一节点上 NFD、Controller、User 和多个 Provider
需要同时存活；Slurm 的 exclusive step 会占用目标节点的全部 CPU/GRES，使后续
同节点 step 排队，形成与 MiniNDN 单机预置环境不同的启动死锁/长期等待边界。

三份 canonical 脚本现统一使用 `--overlap --exact --ntasks=1
--cpus-per-task=1`，继续保留 `--relative=<nodeRank>`；Provider 仍显式请求
`--gpus-per-task=1 --gpu-bind=map_gpu:<gpuRank>`，并由 launcher 做 UUID 核对。
`packaging/...` 兼容路径与 `Experiments/TigerCluster/...` 是同一 hardlink，修改
后内容一致。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| production entry/callers | covered | `run-allocation-topology.sh`, `configure-allocation-routes.sh`, `probe-multinode-network.sh` | `rg -n -- "srun|--exclusive|--relative|gpus-per-task" Experiments/TigerCluster/adapters/slurm-apptainer/scripts` | 所有 topology/route/probe steps 均改为可共存 exact steps；无 exclusive 残留 |
| implementation and wire | covered | `srun_step`, `srun_node`, route/probe command argv | `git diff -- <three scripts>`；对照 `specs/110-itiger-qwen-live-inference/contracts/allocation-topology.md` | 节点 rank、transport、GPU 映射和 launcher argv 保持不变，仅修正 Slurm 资源语义 |
| test/harness/oracle | covered | `test_network_scripts.sh`, `test_allocation_topology.py`, fake `srun` | `python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py`; `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh`; fake-srun 直接拒绝 `--exclusive` | 19/19 topology unit、`NETWORK_SCRIPT_PASS`；静态 grep 覆盖三脚本 |
| build/source closure | N/A | 仅 Slurm shell/harness/docs，无 C++ 或 generated source 变化 | `bash -n` 三脚本与 integration；`python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py` | 无 native build；C++ ABI/source closure 不适用 |
| migration/evidence | covered | Spec110 contract、Spec182 native-first contract、`audit.md`、`tasks.md` | `git diff --check`; `validate_design.py --json`; `audit_speckit_structure.py ... --strict` | G25 与五项多机边界及真实 Slurm 未执行限制已同步，父任务仍 `PARTIAL` |

## Validation

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  19 tests, OK, exit 0
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS, exit 0
bash -n <three canonical scripts> <integration test>
  exit 0
python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py
  exit 0
```

本地 checkpoint 首次提交被仓库 pre-commit 的全局 development-assistant 文本扫描
拦截；这是既有 Spec 文档中的流程引用，不是本批新增路径。按本地 checkpoint 约定以
`NDNSF_LOCAL_CHECKPOINT=1` 重试，同时保留 whitespace 和其余提交检查。

## Batch retrospective

- **static**: `--exclusive` 的整节点资源语义由多机调用链审查发现；此前 fake
  `srun` 没有拒绝该参数，故属于审查新增的控制门。
- **compile/link**: none; 本批未改 C++、Waf target 或 shared library。
- **runtime/test**: 真实 Slurm 尚未运行；fake-srun 集成在修复后通过，不能提升为
  多机资格或协议成功。
- **unobserved**: 端口自动分配、节点内容 digest、exact-SIF canonical runner、
  GPU/SIF、跨节点 NDN request、no-Python 和 T016/T017 仍未观测。

## Closure decision

`CLOSED_FOR_VALIDATION` 仅表示本批 Slurm step resource-sharing 代码/契约/回归
出口稳定；不关闭 R11-B8 maintained callers、R11-B9、T014--T017 或真实
Slurm/SIF 多机资格。下一步继续处理剩余 caller migration 与 no-Python/dependency
closure，并在目标集群执行真实 allocation 验证。
