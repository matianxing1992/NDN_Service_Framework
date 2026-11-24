---
name: itiger-ndnsf-ops
description: Prepare NDNSF SIF delivery and installed deployment, diagnose Tiger SSH, quota, GPU and lifecycle failures, and operate authorized Slurm experiments through maintained tooling.
---

# iTiger NDNSF Operations

## SSH account for this environment

Use `tma1@itiger` for this user's TigerCluster unless the user specifies another
account. Pass the account explicitly; plain `ssh itiger` inherits the local
username `ndn` here and can fail even when the correct account works.
For a bounded read-only check, use:

```bash
ssh -o BatchMode=yes -o ConnectTimeout=12 -o StrictHostKeyChecking=yes \
  tma1@itiger 'id -un; hostname'
```

Before diagnosing missing keys or cluster access failure, verify the effective
SSH user. Rejection of `ndn@itiger` does not establish failure of `tma1@itiger`.
The bundled SSH helpers use the same default. Set `ITIGER_SSH_USER` to change
the account for a bare host alias, or supply an explicit `USER@ALIAS` target.
Keep credentials out of this skill and evidence.

## Locate the task and its owner

Identify the target checkout, authorized workload, current deployment owner and
applicable project rules. Use the task's own inputs to establish model, runtime,
configuration and acceptance identity. The operational guidance below travels
with this skill package.

Respect the current request: documentation or source delivery does not authorize
builds, SSH polling or experiments. Reuse existing authorization for actual runs.
Do not launch historical jobs merely because a guide records them.

## Read the maintained guide for the operation

| Operation | Bundled guidance |
| --- | --- |
| Resource choice, native physical NFD deployment and closeout | [Portable deployment](references/portable-deployment.md) |
| SIF construction, staged publication and storage failures | [SIF build and delivery](references/rootless-container-build.md) |
| Qwen cold/warm, strict CPU oracle and failure lessons | [Qwen experiments](references/qwen-scale-experiments.md) |
| Choosing the smallest check after a failure | [Retry checks](references/pre-tiger-no-repeat-checklist.md) |

Use the installed owner and the application's maintained Waf packer. Inspect
the actual CLI/schema and the failing caller or consumer before choosing options.
The support scripts in this package provide bounded discovery and artifact checks.

## Apply the operational boundaries

- Login nodes handle light editing, control-input transfer, submission and status.
  Builds, model processing, bulk hashing and inference run through Slurm.
- Before every allocation, inspect global nodes, reservations and own jobs.
  Choose any eligible node; CPU work does not wait for free GPUs. Poll within the
  authorized window, normally every 600 seconds, under one operation owner.
- Runtime applications, diagnostics and acceptance probes inside containers are
  C++ only. Python stays on the host for orchestration or offline preparation.
- Reuse the model, complete persistent security state and an unchanged qualified
  SIF. Configuration changes alone do not require a rebuild. Check actual
  container mounts and quota; df and application cache limits are not quota.
- SIF builds use Apptainer 1.5.3 and `--expected-apptainer 1.5.3`; do not weaken
  that check. Build and inspect the actual candidate's native SDK/ELF closure.
- Shared changes pass applicable focused native and CPU cold/warm gates. If the
  task selects physical Tiger only, run those checks on Tiger. Connect the two
  managed NFD instances directly and verify their face, routes and forwarding.
  Before 27B GPU, pass same-candidate Tiger 0.6B GPU cold/warm with independent
  output/state checks, actual CUDA evidence and normal cleanup.
- Separate build, PREPARED, inference, oracle and cleanup results. Keep the first
  failure; repair its smallest owning boundary. Never infer PASS from logs alone.
- Archive bounded reports/statistics and the required audit data. Reclaim only
  explicit idle paths after checking active jobs, mounts and references; preserve
  models, original Repo Data and private state needed for reuse or recovery.
