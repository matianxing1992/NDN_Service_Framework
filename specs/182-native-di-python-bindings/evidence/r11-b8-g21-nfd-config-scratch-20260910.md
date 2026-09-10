# R11-B8-G21 Job-Scoped NFD Configuration

日期：2026-09-10

## Boundary

静态审查发现 v1 process map 的 NFD command 可以携带固定 `/tmp/spec110/...` 配置路径。
MiniNDN 单作业运行通常不会暴露这个约束；并发 Slurm 作业或节点复用时，旧配置可能被读取或
互相覆盖。该批次只修复 topology launcher 的配置路径绑定，不宣称真实多机资格。

## Changes

- `render_process_launcher` 要求 NFD command 恰好包含一个 `--config PATH` 或
  `--config=PATH`，并把它改写成 `$runtime_config`。
- `$runtime_config` 固定为当前 job scratch 下的 `generated/<processId>.conf`；supervisor
  在目标节点把生成配置写入同一路径后才启动 NFD。
- `run-allocation-topology.sh` 不再从 process map 消费 submit-host/fixed `/tmp` 配置路径。

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
19 tests, OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
bash -n .../run-allocation-topology.sh
python3 -m py_compile .../allocation_topology.py
```

The unit fixture retains the historical `/tmp/spec110/nfd-*.conf` command and asserts that the
rendered launcher no longer contains that path. The integration fixture confirms the materialized
launcher and config execute from scratch. No real Slurm/SIF multi-machine qualification was run;
R11-B8 and T016/T017 remain open.
