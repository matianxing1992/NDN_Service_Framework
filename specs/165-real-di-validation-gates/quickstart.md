# Quickstart: Spec 165 Local Gates

## Prerequisites

Run from the repository root. The pinned model snapshot must already exist at:

```text
~/.cache/ndnsf-spec163-hf/hub/models--Qwen--Qwen3-0.6B/snapshots/e6de91484c29aa9480d55605af694f39b081c455
```

The gate does not download a model. MiniNDN/NFD, NDNSF, the selected inference
backend, and the candidate image must be available before execution.

## MiniNDN-first real-model recheck

Before Docker or TigerCluster, run Gate B against the content-addressed
prepared artifacts. This is a real MiniNDN/NFD network with three separate
Provider processes and real Qwen3 ONNX stage weights; `--require-real-model`
fails closed if a fake runtime or incomplete identity/campaign evidence is
substituted.

```bash
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py \
  --gate minindn \
  --output-root results/spec165-minindn-first \
  --artifact-store-root results/_artifacts \
  --reuse-prepared-run \
  results/spec165-local-gates/20260731T074249Z-1edd6ad0
```

For the route profile used by minimal containers or manual remote face
experiments, run the same gate with explicit static routes:

```bash
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py \
  --gate minindn --minindn-routing static \
  --output-root results/spec165-minindn-first-static \
  --artifact-store-root results/_artifacts \
  --reuse-prepared-run \
  results/spec165-local-gates/20260731T074249Z-1edd6ad0
```

Both `nlsr` and `static` are real MiniNDN/NFD profiles; neither uses the fake
runtime.

The accepted profile is CPU-execution evidence (not GPU performance evidence)
and records two prompts, two warmups, six measured requests, eight or more
tokens per request, request-ID lineage, all three selected roles, and per-stage
ONNX artifact/execution markers. Do not replace this with the legacy fake
`llm-pipeline-minindn` smoke.

## Planning validation

```bash
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  --strict specs/165-real-di-validation-gates
```

## Default blocking gate

Build the CPU candidate once when the base image is present; this is a wheel
overlay and does not copy the prepared model artifacts:

```bash
docker build \
  --file packaging/ndnsf-di-container/oci/Dockerfile.spec165-minindn-cpu-gate \
  --tag ndnsf-di:spec165-minindn-cpu-gate .
```

```bash
export NDNSF_DI_CANDIDATE_IMAGE=ndnsf-di:spec165-minindn-cpu-gate
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py \
  --output-root results/spec165-local-gates
```

This is the deployment-authorizing local command only after the current source
revision's MiniNDN-first recheck has passed. It runs Gate A-D, fails on a
missing candidate image or other mandatory prerequisite, writes one JSON
verdict and one matching Markdown summary, and never submits TigerCluster work.

Prepared stage payloads are automatically installed under
`results/_artifacts/qwen-stage-bundles/sha256/<bundle-digest>`. The import uses
hard links and deliberately has no copy fallback. Each run records
`artifact-retention.json` and keeps only a relative
`qwen-onnx-stage-artifacts` link, so future runs do not depend on an older run
directory and do not allocate another 5--6 GB.

The passing current-source aggregate is
`results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8`; it uses the
CPU-only candidate image digest
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`.
The older CUDA-wheel image `ndnsf-di:spec165-minindn-gate` is retained only as
negative performance evidence: its CPU fallback timed out under the same
three-provider workload. Keep the CPU image identity and workload digest
immutable when rerunning or promoting evidence.

## Diagnostic subsets

Subsets may diagnose a failure but cannot authorize deployment:

```bash
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --gate fidelity
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --gate deadline
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --gate minindn
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py --gate container
```

The exact final flags are implementation-owned; if they change, this contract
and CLI help must change together.

## Required result layout

```text
<run-directory>/
├── aggregate-verdict.json
├── artifact-retention.json
├── summary.md
├── workload.json
├── a/
├── b/
├── c/
├── d/
├── minindn/
│   └── runtime/qwen-onnx-stage-artifacts -> ../../../../_artifacts/...
└── container/
```

Do not copy old evidence into a new run directory. Do not treat `SKIP` as a
passing mandatory gate.

## TigerCluster boundary

A local PASS only makes external validation eligible. It does not submit a
job. After the strict MiniNDN profile or any harness change, rerun the full
aggregate and require `externalValidationAuthorized: true`; the Gate-B subset
alone does not authorize Docker or TigerCluster. TigerCluster remains a
separate explicit command.
