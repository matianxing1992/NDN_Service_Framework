# R11-B8-G22 Pre-Start Failure Evidence

日期：2026-09-10

## Boundary

多机 launcher 在 `--workdir` 可见性检查、目标节点 materialization 或 process-map render
阶段失败时尚未启动子进程。旧实现直接退出，缺少 `teardown.json`，使启动前失败无法与未执行
区分。该批次只修复失败证据，不宣称真实 Slurm/SIF 资格。

## Changes

- supervisor 在生成 map 后、任何 `srun` preflight 前安装 pre-start `EXIT/INT/TERM` traps。
- 预检失败写入原始退出码、`survivors: 0` 和 `status: FAIL`，随后保留原退出状态。
- 进入 NFD 启动阶段前撤销 pre-start traps，继续使用现有进程组 teardown/audit。

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
19 tests, OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
bash -n .../run-allocation-topology.sh
python3 -m py_compile .../allocation_topology.py
```

The integration fixture injects a failed per-node workdir check and verifies exit code `4` plus a
zero-survivor `teardown.json`; normal and TERM paths remain covered. No real Slurm/SIF multi-machine
qualification was run; R11-B8 and T016/T017 remain open.
