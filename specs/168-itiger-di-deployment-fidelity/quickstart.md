# Spec 168 Operator Quickstart

This is an admission workflow, not a command to immediately submit a remote job.
Current artifacts from Specs 162 and 167 are reused when their hashes match.

## 1. Confirm the active feature and tools

```bash
cd /home/tianxing/NDN/ndn-service-framework
jq . .specify/feature.json
python3 scripts/context_mode_guard.py health --project-root .
codegraph status .
node /home/tianxing/.codex/gsd-core/bin/gsd-tools.cjs validate health
git status --short
```

Expected: Spec 168 is active, repository Context Mode and CodeGraph are healthy,
and any existing dirty files are identified and preserved.

## 2. Read the admission authority

```bash
sed -n '1,260p' specs/168-itiger-di-deployment-fidelity/spec.md
sed -n '1,320p' specs/168-itiger-di-deployment-fidelity/plan.md
sed -n '1,260p' specs/168-itiger-di-deployment-fidelity/contracts/experiment-contract.md
```

Do not change or delete the prior job 181948/181951 evidence. Do not rebuild a
foundation image or reprepare model shards only to create a fresh timestamp.

## 3. Run existing focused controls

The exact final Gate A command will be frozen in the Spec 168 candidate manifest.
The existing focused controls include:

```bash
python3 -m pytest -q \
  tests/python/test_ndnsf_di_selection_dataflow.py \
  tests/python/test_spec165_progress_deadline.py \
  tests/python/test_spec161_qwen_generation.py
```

After implementation, add the Spec 168 regression file to the same command. A
test pass is necessary but is not a substitute for real MiniNDN.

## 4. Run the deployment-faithful local gates

The implementation phase must add one canonical Spec 168 launcher under
`specs/168-itiger-di-deployment-fidelity/jobs/`. It must:

1. launch real MiniNDN/NFD plus independent Controller, Repository, User, and
   three Provider processes;
2. use normal security, routes, DistributedRepo fetch, request API, Qwen adapter,
   progress deadlines, and lifecycle evidence;
3. generate a complete multi-token answer;
4. rerun inside the exact candidate SIF without test-only defaults; when the
   development host lacks CUDA/Apptainer, retain the local result as explicit
   `CPU_LOGIC` and run a bounded single-node TigerCluster `CUDA` preflight before
   any three-node campaign;
5. write a machine-readable admission manifest and verdict.

The development host has 8 GiB RAM. Local execution is therefore restricted to
the tiny-Qwen profile under the 6 GiB memory / 7 GiB memory-plus-swap cgroup or
to non-materializing metadata and state-machine tests. Do not fetch or load the
full Qwen3-0.6B or large-model weights locally. CUDA, high-memory, and workloads
without a demonstrated bounded peak belong in a TigerCluster Slurm allocation.

Until that launcher exists and Gates A-D pass, do not submit a new three-node
TigerCluster campaign. A single-node exact-SIF CUDA preflight is Gate C, not a
distributed-inference campaign or success claim.

## 5. Reuse existing remote material

The retained Spec 162 infrastructure contains the current generation assets and
jobs, including:

```text
specs/162-itiger-qwen36-generation/jobs/generation-smoke.sbatch
specs/162-itiger-qwen36-generation/jobs/generation-rank-inner.sh
specs/162-itiger-qwen36-generation/jobs/run-repo-node.py
specs/162-itiger-qwen36-generation/jobs/analyze-generation-full-smoke.py
```

Spec 168 may wrap or parameterize these after identity verification. It must not
copy 5-20 GB payloads into each run directory or generate a parallel inference
implementation.

## 6. Remote submission sequence

After explicit candidate admission:

1. submit exactly one small-model single-request control;
2. inspect the retained terminal Response and lifecycle verdict;
3. only if it passes, submit the frozen 30-row small-model repeated schedule;
4. only if all rows reconcile, submit one large-model single request;
5. stop at the first failure and return to the local reproducer/repair chain.

Use Slurm for all compute. Login nodes are for inspection, transfer, and job
submission only. There is no 300-second Provider settle window: route/process
bootstrap is checked, then the User Request drives ACK, planning, preparation,
and stage execution.

## 7. Acceptance checklist

- one request ID from Request to terminal Response;
- ACK closure before graph-aware partition/placement;
- committed immutable plan and final Selection;
- one provider-specific Selection projection per collaboration role, all bound
  to the same request/attempt and within the admitted protected wire budget;
- model bytes acquired through DistributedRepo, not shared-storage injection;
- verified disk/RAM/GPU transitions;
- Stage 0 starts on local readiness; later stages start on local readiness plus
  direct predecessor data;
- one collaboration for the complete multi-token answer;
- zero CPU fallback and authenticated terminal Response;
- later compatible requests show zero duplicate transfer and device load;
- every schedule row and failure is retained and classified.

## 8. Failure handoff

On failure, record the experiment/job, request, last progress checkpoint, primary
taxonomy code, owner, raw evidence path, and whether the same candidate fails in
real MiniNDN or the exact container. Do not silently rerun. A code repair creates
a new linked source identity and must pass local admission before remote use.
