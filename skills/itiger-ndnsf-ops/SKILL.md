---
name: itiger-ndnsf-ops
description: Prepare NDNSF SIF delivery and installed deployment, diagnose Tiger SSH, quota, GPU and lifecycle failures, and operate authorized Slurm experiments through maintained tooling.
---

# iTiger NDNSF Operations

## Locate the task and its owner

Run `git rev-parse --show-toplevel` in the target checkout. Resolve every repository
path below from that root, never from the installed personal skill directory.
Read its AGENTS.md and the selected feature's relevant contracts/tasks/evidence.
Optional historical files may be absent in a fresh checkout.

Respect the current request: documentation or source delivery does not authorize
builds, SSH polling or experiments. Reuse existing authorization for actual runs.
Do not launch historical jobs merely because a guide records them.

## Read the maintained guide for the operation

| Operation | Repository path |
| --- | --- |
| Resource choice, prepare/use, persistent state, batches, SSH and closeout | `Experiments/TigerCluster/docs/portable-deployment.md` |
| Base/application SIF build, checkpoint reuse and candidate qualification | `Experiments/TigerCluster/docs/sif-build.md` |
| Historical results, resource measurements and known failure boundaries | `Experiments/TigerCluster/docs/lessons-and-evidence.md` |
| Installed MiniNDN CPU gate | `Experiments/guides/local-validation.md` |
| Installation and incremental native development | `Experiments/guides/installation.md`, `local-development.md` in the same directory |
| Maintained scripts and ownership | `Experiments/TigerCluster/README.md` |

Experiments holds the maintained operational guidance; project rules remain in AGENTS.md.
Legacy files in references/ are migration pointers, not additional checklists.
Inspect the actual CLI/schema and first caller/consumer before using an option.
Use the existing cluster owner and Waf packer; do not add a parallel launcher.

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
- Shared changes pass focused native checks and installed MiniNDN CPU cold/warm.
  Before 27B GPU, pass same-candidate Tiger 0.6B GPU cold/warm with independent
  output/state checks, actual CUDA evidence and normal cleanup.
- Separate build, PREPARED, inference, oracle and cleanup results. Keep the first
  failure; repair its smallest owning boundary. Never infer PASS from logs alone.
- Archive bounded reports/statistics and the required audit data. Reclaim only
  explicit idle paths after checking active jobs, mounts and references; preserve
  models, original Repo Data and private state needed for reuse or recovery.
