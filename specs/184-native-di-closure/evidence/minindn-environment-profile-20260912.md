# Spec184 MiniNDN Environment Profile Evidence

**Status**: `STATIC_PASS / READY_FOR_BATCH_TESTS` for the environment-input
unit; native behavior and qualification remain `PARTIAL`.

本单元解决本地脚本与实际机器之间的环境差异。协议请求路径仍由
`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` 维护；拓扑文件、Provider 节点、
MiniNDN home root、模型缓存、应用状态根和 native Provider 可执行文件可由同一份
`ndnsf-di-minindn-environment-v1` profile 提供，显式 CLI 参数优先。启动前检查拓扑
节点和 native binary，并在输出目录保存 profile digest 与解析结果。Spec175 SIF
wrapper 也使用 profile 的 MiniNDN root，不再固定 `/tmp/minindn`。

源码 checkpoint：`689cca00d40b1e2bf241aff4e7d14b0204a7c892`。
示例 profile：[ndnsf-di-minindn-local.example.json](../../../Experiments/profiles/ndnsf-di-minindn-local.example.json)。
操作说明：[MiniNDN environment profile](../../../docs/ndnsf-di-minindn-environment.md)。

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `main`, `build_parser`, `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` environment delegation | `rg -n 'build_delegate_argv|os.execvpe|NDNSF_DI_MININDN_PROFILE' Experiments tests` | Profile is inherited by the maintained delegated runner; CLI remains the explicit override. |
| `implementation and wire` | `covered` | `load_environment_profile`, `apply_environment_profile`, `validate_environment_inputs`, `sif_exec_prefix`, `_install_sif_command_wrappers` | CodeGraph exploration plus diff review; `git diff --check`; shell quoting and topology-node mapping inspection | Static review found and fixed the SIF wrapper's hard-coded `/tmp/minindn`; profile root now reaches bind/home arguments. |
| `test/harness/oracle` | `covered` | `tests/python/test_spec184_minindn_environment_profile.py` | `python3 -m pytest -q tests/python/test_spec180_qwen_entrypoint.py tests/python/test_spec184_minindn_environment_profile.py tests/python/test_spec175_minindn_layout.py` | `17 passed`; tests cover strict schema, path resolution, CLI precedence, topology preflight and unsafe/duplicate node rejection. |
| `build/source closure` | `covered` | Python runner and profile example; no C++ target/source change | `python3 -m py_compile ...`; `python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py --help` | `compile/link` is not applicable to this Python-only unit; native binary existence is checked before runtime startup. |
| `migration/evidence` | `covered` | Local example, resolved profile record, Spec184 T007 evidence | `git show --stat 689cca00`; profile marker/record code review | Same runner command is retained; actual remote/Tiger SIF and Qwen3.6-27B qualification remain external gates. |

## Review trace and retrospective

The read-only review loaded `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`),
the project pre-test review rules (SHA-256
`6b5aff9458d1da93a6b0021ca5713aba58c14006654edef28744ed445259b486`), the
complete checkpoint diff, the maintained Qwen delegator and existing MiniNDN
layout tests. Review finding: SIF command wrappers ignored the configured
MiniNDN root; corrected before the batch test. No remaining control finding.

- `static`: PASS after the wrapper-root re-review.
- `compile/link`: Python syntax/entrypoint checks passed; no native build was needed.
- `runtime/test`: focused Python/layout selectors passed (`17/17`). Existing broader
  Spec168/Spec175 collection has unrelated baseline failures (missing `build/unit-tests`,
  historical source-contract assertions); those are not attributed to this unit.
- `unobserved`: a real MiniNDN run with a non-default profile, remote machine run,
  SIF execution, and model qualification were not started in this documentation/tooling
  unit. They remain `PARTIAL`/external evidence gates.

**Closure decision**: `CLOSED_FOR_VALIDATION` for the environment-input unit;
Spec184 T007 remains `IN_PROGRESS / PARTIAL`.
