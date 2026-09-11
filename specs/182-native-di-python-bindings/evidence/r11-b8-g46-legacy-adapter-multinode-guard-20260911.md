# R11-B8-G46 Legacy Slurm Multi-node Adapter Guard

**Date**: 2026-09-11
**Status**: `CLOSED_FOR_VALIDATION` for the bounded deployment guard; not `QUALIFICATION_PASS`
**Scope**: `packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py`

## Finding

静态追踪发现旧 `SlurmApptainerAdapter` 可以接受 `slurm.nodes > 1` 的 profile，但
`ndnsf-di.sbatch.in` 始终只调用一次 `run-container.sh`。该入口不读取 `process-map`，不调用
`run-allocation-topology.sh`，也不建立跨节点 NFD faces/routes。调度器因此可能报告多节点
allocation，而实际业务只在一个 container 中运行；MiniNDN 单机不会触发该分叉。

`submit()` 原先在渲染模板之后才暴露这个问题，但先创建了 run state directory。失败后重试会
误报 `SLURM_RUN_ALREADY_SUBMITTED`，掩盖真正的拓扑入口错误。

## Correction

- `render_sbatch()` 和 `submit()` 对 `nodes > 1` 返回
  `SLURM_MULTINODE_TOPOLOGY_RUNNER_REQUIRED`，明确要求使用 topology launcher。
- `submit()` 在 preflight、SIF materialization 和 state-directory 创建之前执行该 guard。
- 单节点旧兼容入口保持原行为；Spec110 topology launcher 仍是多机执行的唯一入口。

## Static review

**Review trace**: 官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；基线
`735190dcd96ceeb2c`；审查范围为该基线后的完整未提交差异。实际查询包括
`codegraph node packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py`、
`rg -n "render_sbatch|SlurmApptainerAdapter|ndnsf-di.sbatch.in|run-allocation-topology" packaging/ndnsf-di-container tests/container`
以及 `git diff --check`。

官方 review-agent 结论：`No findings.` 本次实现没有发现由该差异引入的 correctness、security、
performance 或 maintainability regression。设计边界仍明确保留真实多机资格为后续 obligation。

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | covered | `SlurmApptainerAdapter.submit`, `render_sbatch`, `ndnsf-di.sbatch.in`, `run-allocation-topology.sh` | CodeGraph node; `rg -n "render_sbatch|SlurmApptainerAdapter|run-allocation-topology" ...` | confirmed legacy one-container path and explicit topology-runner boundary |
| `implementation and wire` | covered | `_require_single_node_legacy_path`; submit preflight/materialization/state ordering | source diff; `python3 -m py_compile ...` | guard is shared and runs before external commands; no finding |
| `test/harness/oracle` | covered | `test_slurm_render.py`, `test_slurm_submit.py`, allocation topology/network integration | `pytest` selectors; `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` | schema-valid multi-node negative and no-side-effect oracle pass |
| `build/source closure` | N/A | Python adapter module; no native target or link source changed | `py_compile`; Spec Kit sync validator | N/A because this unit has no C++/ABI/build-target change; no native build claimed |
| `migration/evidence` | covered | native-first contract, `audit.md`, `tasks.md`, this evidence | `verify-spec-kit-sync.py`; `audit_speckit_structure.py`; `git diff --check` | legacy path now fails closed; true multi-node/no-Python/T016 remains open |

**Changed gate**: added a pre-external-command multi-node guard and a no-state-side-effect assertion
after the static review exposed the legacy adapter/template mismatch. No compile/link or runtime miss was
reclassified; the new guard is the changed deployment gate.

## Five-lane evidence

| Lane | Evidence |
| --- | --- |
| Production entry/callers | `SlurmApptainerAdapter.submit()` → `render_sbatch()` → `ndnsf-di.sbatch.in` only had one `run-container.sh`; canonical `run-allocation-topology.sh` was not called. |
| Implementation/wire | Guard is shared by render and submit; submit checks it before external commands and state creation. |
| Test/harness/oracle | `tests/container/unit/test_slurm_render.py` rejects a schema-valid multi-node profile; `tests/container/unit/test_slurm_submit.py` proves zero runner calls and no state directory. |
| Build/source closure | Python-only adapter change; `py_compile` and Spec Kit/design validators were run; no native binary was rebuilt. |
| Migration/evidence | This contract, audit entry and `tasks.md` checkpoint record the boundary. True multi-node/no-Python/exact-SIF qualification remains open. |

## Validation

Commands and results for this checkpoint:

```text
python3 -m pytest -q tests/container/unit/test_slurm_render.py tests/container/unit/test_slurm_submit.py
PASS (all tests)
python3 -m py_compile packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py
PASS
python3 -m pytest -q tests/container/itiger-qwen-live/unit/test_allocation_topology.py
PASS
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
PASS
python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --repo-root . --require-entrypoints
PASS
git diff --check
PASS
```

这些检查只证明旧入口不会静默吞掉多节点声明；不证明真实 Slurm allocation、SIF、NDN TCP/UDP
route、跨进程 C++ requester/Core/Provider 或 T016/T017 资格。

## Batch retrospective

- **static**: found the false-green legacy multi-node dispatch and the retry-masking state-directory
  side effect before batch tests; both were corrected in this unit.
- **compile/link**: not applicable; this is a Python adapter and documentation change, with no native
  target or source-list mutation.
- **runtime/test**: render/submit tests, topology unit tests and network-script integration passed;
  the network script retains bounded fake-launcher `BrokenPipeError` output while returning
  `NETWORK_SCRIPT_PASS`.
- **unobserved**: real Slurm allocation, remote node visibility, SIF/GPU, NDN TCP/UDP protocol traffic,
  C++ no-Python requester/Core/Provider and T016/T017 qualification remain unobserved.

## Next

继续按 [native-first execution order](../contracts/native-first-execution.md) 处理
R11-B8 maintained caller migration 或 R11-B9 closure；多机真实执行必须从 canonical
topology launcher 进入，并保留 no-Python 与 T016 资格边界。
