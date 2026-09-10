# R11-B8-G23 Early Input Failure Evidence

日期：2026-09-10

## Boundary

G22 的 pre-start trap 已覆盖 map render、workdir visibility 和 launcher materialization，但
trap 安装仍位于 workdir/scratch/allocation-mode 检查之后。无效输入因此可能没有 teardown
证据。该批次只扩大失败证据覆盖，不宣称真实多机资格。

## Changes

在 process map 和 NFD template 文件确认后立即创建 evidence 目录并安装 `EXIT/INT/TERM`
pre-start traps，再执行 workdir、allocation、scratch 和后续 map-render 检查。已有 regular
child teardown 在第一个 NFD 启动前接管并撤销 pre-start traps。

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
19 tests, OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
bash -n .../run-allocation-topology.sh
python3 -m py_compile .../allocation_topology.py
```

The integration fixture passes a relative workdir, verifies exit code `3`, and checks the emitted
zero-survivor `teardown.json`; the injected visibility failure and normal/TERM paths remain green.
No real Slurm/SIF multi-machine qualification was run; R11-B8 and T016/T017 remain open.
