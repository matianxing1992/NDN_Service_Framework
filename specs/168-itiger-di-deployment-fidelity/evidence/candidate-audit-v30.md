# Spec 168 v30 Candidate Audit

## Verdict

**BLOCK for Gate D and every three-node TigerCluster campaign.** Gates A-C are
accepted for source
`sha256:9cc623472357d0f0f52e2f6fc669f82eb072b2e6dd5c5b63f88e3bde55b1dae4`,
but the repository does not yet contain a frozen Spec 168 three-node
small-model launcher and matching post-run analyzer. No Gate E job is admitted.

## Accepted evidence

- Spec Kit strict structure: PASS; 24 functional requirements, 11 success
  criteria, 17 tasks, and 8 completed tasks at audit time.
- Spec Kit prerequisites: PASS for `spec.md`, `plan.md`, and `tasks.md`.
- Gate A focused regressions: 24 Selection, 15 automatic planning, 9 Provider
  generation, 9 real-MiniNDN gate, 6 campaign, 3 exact-SIF preflight/bundle,
  and 3 tiny-Qwen tests pass in their required environments.
- Gate B: PASS in 92,230.833 ms under 6 GiB memory / 7 GiB memory-plus-swap;
  four tokens, 20 lifecycle events, and no observed cgroup OOM.
- Gate C negative lineage: job 182382 retained an exact-container partial
  package-overlay failure before CUDA/model load.
- Gate C replacement: job 182384 completed in 80 seconds on one RTX 5000;
  exact SIF/source/model/stage identities matched, all three CUDA stages loaded
  and warmed without CPU fallback, and top token 8065 matched the reference.
- Campaign V3 contract: six tests pass. The real-baseline metadata dry run
  derived/froze/validated/claimed one payload-free campaign identity and then
  deleted the temporary manifests.

## Blocking findings

### D-001 - No native Spec 168 three-node launch surface (HIGH)

`specs/168-itiger-di-deployment-fidelity/jobs/` has a bounded Gate C sbatch but
no three-node Request-first launcher that starts Controller, Repo, User, and
three Providers, preserves one request ID, performs post-ACK planning, and
collects a complete authenticated Response. Reusing the Spec 162 rank script
without an explicit adapter would reintroduce older source-overlay and lifecycle
assumptions. This can invalidate the central deployment claim.

### D-002 - Remote analyzer/schedule inputs are not executable identities (HIGH)

The V3 dry-run bindings deliberately use the current lifecycle evidence module,
the inherited Spec 162 rank script, and the campaign tool as metadata inputs.
They prove deterministic V3 identity construction only. They are not a frozen
Spec 168 remote analyzer, route launcher, or 30-row schedule. Therefore the dry
run must not be promoted into a formal campaign manifest.

### D-003 - Remote lifecycle evidence is not yet wired to the V3 gate (HIGH)

The accepted local analyzer validates request/plan/artifact/boot bindings, but
no Spec 168 Slurm wrapper currently reconciles those fields from all three
nodes into one immutable admission result. A successful user process alone
would repeat job 181948's “inference sub-result inside failed campaign” ambiguity.

## Required closure

1. Implement one Spec 168 three-node RTX5000 launcher by adapting, not silently
   reusing, the measured Spec 162 job; remove fixed Provider settle behavior and
   preserve request-driven ACK/planning/preparation/dataflow.
2. Implement the matching analyzer and machine-readable route/GPU/process and
   lifecycle evidence inputs.
3. Run their static/focused contract tests locally without loading the remote
   model.
4. Recompute `routeDigest`, `analyzerDigest`, and `scheduleDigest`; freeze one V3
   campaign and claim one unique result directory.
5. Only then submit exactly one Gate E Qwen3-0.6B three-node job.

## Five-tool gate report

- Context Mode: health/active-Spec checks were used earlier; repository files
  remain authority.
- CodeGraph: current runtime closure and Qwen load/warm/forward behavior were
  verified and the index was synchronized.
- Spec Kit: strict structure and prerequisites pass; audit verdict is BLOCK.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable to this implementation/admission task; no literature or
  statistical research claim was made.
