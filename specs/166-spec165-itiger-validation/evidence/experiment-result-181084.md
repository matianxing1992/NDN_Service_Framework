## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: VERIFIED FAILURE
- Version Label: exp_result_v1

## Experiment Result

- **ID**: `spec166-standalone-qwen3-004`
- **Type**: standalone RTX 5000 GPU reference
- **Status**: crashed
- **Slurm Job**: `181084`
- **Node**: `itiger07`
- **Duration**: 54 seconds
- **Exit Code**: `1:0`
- **Source**: immutable `/project/tma1/ndnsf-di/jobs/spec166/source-004`
- **Source manifest SHA-256**:
  `a2ceca2f1036ab6c426a0a9b6332f552047af8131f8cc5ed24594b8b8cfa70ec`
- **Evidence**:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/.spec166-standalone-qwen3-004.partial`

### Result

All eight preregistered generations completed with exactly eight tokens and
matched the frozen reference sequences. CUDA was selected first with no model
CPU fallback. The terminal failure was `STANDALONE_EP_PROFILE_POLICY_MISMATCH`.

All three profiles showed CUDA `MatMul`, `Softmax`, and
`SimplifiedLayerNormalization`. Every CPU event used only `int64` or `bool`
inputs/outputs, with at most five elements; there were zero floating CPU events.
Thus the observed CPU partition is dynamic shape/control computation, while
v1 incorrectly rejected its Add/Concat/Div/Equal/Gather/Mul/Split/Squeeze/Where
operator names and incorrectly required an unfused `ReduceMean` event.

The failure is preserved and is not retroactively relabeled. It motivates the
v2 operator-plus-tensor-evidence contract. No three-node job was submitted.
