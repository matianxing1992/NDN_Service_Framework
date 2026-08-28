## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: run
- Origin Date: 2026-07-31
- Verification Status: UNVERIFIED
- Version Label: spec166_exp_plan_v1

## Experiment

- Independent variable: execution substrate (`standalone one-node CUDA`
  followed by `three-node NDNSF-DI CUDA`).
- Controlled variables: image, model/revision, model/workload digests, prompts,
  token count, greedy decoding, ONNX artifacts, node/GPU class, and source
  bundle.
- Experimental unit: one complete generation invocation.
- Sample: two warmups excluded from measurement and six measured invocations,
  retained independently.
- Primary correctness outcome: exact reference-token equality.
- Secondary outcomes: completion, TTFT, per-token latency, total latency,
  tokens/s, stage timing, dependency bytes/digests, and request lineage.
- Failure accounting: every preregistered row and job remains in evidence;
  missing data, CPU fallback, mismatch, timeout, OOM, or scheduler failure is a
  failure rather than a skip.
- Monitoring: Slurm/process state plus output/progress growth every 30–60
  seconds; three unchanged checks are advisory only; the Slurm walltime is the
  hard terminal bound.
- Statistical boundary: six measured rows support functional repeatability,
  not comparative performance inference.
