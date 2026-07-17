# Validation Quickstart

This document defines the implementation validation sequence; commands become
executable as the corresponding tasks land.

## 1. Static and Focused Gates

```bash
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
python3 tests/python/test_ndnsf_di_deployment_readiness.py
./build/unit-tests \
  '--run_test=ExecutionEvidence*,ProviderResourceProbe*,DependencyWaitScheduler*,ProviderRoleWorker*,OnnxRuntime*' \
  --report_level=detailed --log_level=nothing
```

Expected: structure PASS; all focused tests pass; synthetic evidence fails the
real-compute fixture; 1,000 waits remain bounded.

## 2. Evidence Truth Cells

```bash
python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --policy-bundle llm-proportional \
  --tracer-deterministic-runner \
  --out results/spec105-evidence-synthetic-<unique>

python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-onnx-cpu \
  --output-dir results/spec105-evidence-real-cpu-<unique> \
  --measured-requests 2
```

Expected: first release gate BLOCK with `synthetic-delay`; second reports
provider-observed CPU ONNX evidence. A separate local CUDA-request fixture must
fail closed because this host has no CUDA provider. Neither directory is reused.

## 3. Frozen MiniNDN Acceptance

Run the single-node baseline and three distributed repetitions using the
campaign profile produced by the implementation. Every measured repetition is
60 seconds, 1 RPS, <=512 input tokens, 32 greedy output tokens, INFO logging and
the normal MiniNDN application-security path.

```bash
ndnsf-di bench \
  --campaign examples/ndnsf-di-qwen-pilot-minindn.campaign.json \
  --out results/spec105-qwen-pilot-<unique>
```

Expected: token equality; >=99% completion; >=95% offered throughput; p95 ratio
<=2.0; complete stage/resource metrics; release gate PASS for evidence,
correctness and MiniNDN performance. Cryptographic-strength security remains
DEFERRED to Spec 106 and is never inferred from the dummy keychain.

## 4. Fault Acceptance

```bash
ndnsf-di bench \
  --campaign examples/ndnsf-di-qwen-pilot-faults.campaign.json \
  --out results/spec105-qwen-faults-<unique>
```

Expected: five fixed provider-loss treatments plus deterministic negative cells,
one terminal outcome per request, at most one replacement, no stale authority,
all failed cells retained.

## 5. Local Deployment Candidate

Only after all prior MiniNDN dimensions PASS:

```bash
sudo packaging/ndnsf-di-systemd/install.sh --release <release-dir>
ndnsf-di doctor --profile /etc/ndnsf-di/deployment.json --json
sudo systemctl start ndnsf-di-controller.target
sudo systemctl start ndnsf-di-providers.target
ndnsf-di status --profile /etc/ndnsf-di/deployment.json --json
```

Follow the generated local operator runbook for clean-staging canary, provider
restart, staged upgrade, rollback and 24-hour MiniNDN soak. The resulting gate
may set `minindnCandidateOverall=PASS`; it must keep
`physicalProductionOverall=DEFERRED` until Spec 106 executes.
