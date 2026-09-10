# R11-B8-G13 Multi-Machine Deployment Boundary

## Scope and decision

本批只修复多机 Slurm topology 的进程启动边界，不推进 maintained callers、no-Python、
依赖闭包或 T016/T017 资格。MiniNDN 中常见的预置 HOME、PIB/TPM 和工作目录不能作为多机
部署证据；本批以生成脚本和真实 fake-process 执行为准。

## Finding and repair

静态审查发现 `run-allocation-topology.sh` 只把 `identityRef` 留在 frozen process map，
生成的脚本没有消费它，进程会继承 Slurm/login 环境中的 `HOME`、`NDN_CLIENT_PIB` 和
`NDN_CLIENT_TPM`。`identityReadOnly=true` 又禁止把 `/project/.../identities/<role>`
直接当作可写 keychain。相同脚本也没有固定当前工作目录，因此 `python3 user.py`、模型
文件和相对配置会相对提交者目录解析。

修复集中在 `allocation_topology.render_process_launcher` 和
`run-allocation-topology.sh`：

- `--workdir` 现在是必填的绝对目录；每个节点的生成脚本先验证并 `cd` 到它。
- 每个 Controller、Provider、User 在节点 scratch 建立独立的 mode-0700 HOME，复制各自
  只读 `identityRef`，并要求 `.ndn/pib.db` 与 `.ndn/ndnsec-key-file`。
- 脚本先清除继承的 NDN keychain 变量，再显式导出与该 HOME 匹配的 PIB/TPM 和节点 NFD
  UNIX transport。NFD 使用自己的 scratch HOME，并保持无身份配置。
- 缺少身份源、PIB/TPM 或工作目录发生在 `exec` 之前；`NDNSF_SPEC110_TEST_MODE=1` 只
  允许离线 fake-binary fixture 跳过不存在的 `/project` 身份源，不能用于真实部署或资格。
- process ID、identityRef、scratch 和 workdir 拒绝 `..` 路径组件，避免生成脚本或 scratch
  清理越出作业目录。

设计同步到 [Spec110 allocation topology contract](../../110-itiger-qwen-live-inference/contracts/allocation-topology.md)、
[Spec182 native-first execution](../contracts/native-first-execution.md) 和
[Tiger baseline runbook](../../../Experiments/TigerCluster/docs/two-node-baseline.md)。

## Five-lane review

| Lane | Result | Evidence |
| --- | --- | --- |
| Production entry / callers | PASS | `run-allocation-topology.sh` now routes every generated process through `render_process_launcher`; no caller bypass was added. |
| Implementation / wire | PASS | Per-process HOME, PIB, TPM, transport, scratch TMPDIR and explicit workdir are rendered before `exec`; NFD keychain variables are cleared. |
| Test / harness / oracle | PASS | Unit rendering, `bash -n`, and a real fake provider that records inherited/replaced environment and copied PIB bytes. |
| Build / source closure | N/A | No C++ or shared-library source changed; this is a packaging/launcher boundary. |
| Migration / evidence | PARTIAL | Spec110/Spec182 contracts and task ledger are synchronized; real multi-node SIF, route, identity and dependency closure qualification remains open. |

## Verification

Commands run from the repository root:

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  12 tests, OK
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  exit 0
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS
```

The fake provider started with ambient `HOME`, `NDN_CLIENT_PIB`, and `NDN_CLIENT_TPM`; it
observed the process-specific scratch values, the expected NFD transport, and a copied `pib.db`
with the original bytes. The network script's fake Slurm path completed with zero survivors.
No real Slurm allocation, SIF, multi-node route, or MiniNDN run was performed in this batch.

The complete `tests/container/itiger-qwen-live/unit` discovery was also attempted (109 tests).
It retained three pre-existing `test_rootless_build` errors at the
`ROOTLESS_BUILD_TEMPLATE_UNRESOLVED` boundary; those tests do not import the changed topology
launcher and are outside this card. They are not counted as topology failures or as a qualification
result.

## Residual deployment boundary

The separate R10-B82/R10-B79 finding that locally built ELF artifacts resolve host-specific
NAC-ABE/SVS/ONNX/NDN libraries remains open. This launcher repair does not turn a host build tree
into a portable SIF; T014/T015/T016 must still build and inspect the complete candidate closure
inside the declared image/bundle boundary.
