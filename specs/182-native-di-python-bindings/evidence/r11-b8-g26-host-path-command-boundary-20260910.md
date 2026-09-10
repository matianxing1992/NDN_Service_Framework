# R11-B8-G26 Host Path Command Boundary

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded multi-node launcher pre-exec gate
**Parent**: R11-B8 / T014 / Spec110 allocation topology

## Finding and repair

静态审查发现 v1 process map 只检查命令 token 能安全解析，未阻止应用参数继续携带提交主机
或构建机路径。跨节点时，`--model=/project/...`、`--cache=/home/...` 这类路径会在不同节点
解析到错误内容或不存在；错误的 `--identity /project/...` 还会绕过 launcher 已复制的进程身份。

`allocation_topology.py` 现在在 map validation 和直接 launcher rendering 两个入口拒绝
`/home/`、`/project/`、`/workspace/`、`/build/`、`/src/`、`/tmp/` 宿主/构建前缀。唯一允许的
例外是精确的声明 `identityRef`（随后重写到进程专属 `HOME`），以及 NFD 的 `--config` 参数
（随后重写到当前 job scratch）。这样问题停在 pre-exec，而不是等到多机业务请求阶段才暴露。

## Validation

- `python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py`：20/20 通过，新增
  application host-path 与错误 identity 参数反例。
- `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh`：
  `NETWORK_SCRIPT_PASS`。
- `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py`：通过。
- `bash -n`：Tiger topology、route、network probe 三个脚本通过。

该门只验证命令路径不会隐式依赖提交节点。它不替代内容 digest、exact-SIF/ELF、真实
Slurm/GPU、跨节点 NDN request、no-Python 或 T016/T017 资格。
