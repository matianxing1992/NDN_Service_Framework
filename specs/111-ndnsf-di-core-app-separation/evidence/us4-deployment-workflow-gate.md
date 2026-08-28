# US4 Deployment and Durable Invocation Gate

Date: 2026-07-14  
Verdict: **PASS**

This evidence supersedes the earlier local-lambda gate. One real MiniNDN run
started the existing ServiceController, three APPProvider processes and one
APPClient. Ordinary NDNSF services carried APP-layer Ed25519 Provider evidence;
no new framework wire type was introduced.

The run completed:

```text
validate/resolve/dry-run
-> Provider PREPARE and signed revision/boot/artifact readiness (3 roles)
-> Provider ACTIVATE receipts (3 roles)
-> APPDeployment ACTIVE and restart recovery
-> durable distributed inference and APPClient reopen
-> Provider DRAIN receipts and INACTIVE
-> rollback as lifecycle epoch 2
-> PREPARE/ACTIVATE epoch-2 revision
-> final DRAIN/DELETE receipts
-> APPDeployment restart recovers DELETED
```

MiniNDN command:

```bash
sudo -n -E env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  LD_LIBRARY_PATH="$PWD/build" \
  timeout 300s python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --output-dir results/spec111-deployment-workflow-smoke-20260714-retry2 \
  --campaign-id spec111-deployment-workflow-smoke-20260714-retry2 \
  --runtime fake --stages 3 --layers 24 --compute-delay-ms 1 \
  --warmup-requests 0 --measured-requests 1 --measured-duration-s 0 \
  --request-interval-ms 0 --max-new-tokens 1 \
  --ack-timeout-ms 1500 --timeout-ms 60000 \
  --ndn-log 'ndn_service_framework.*=WARN' \
  --deployment-workflow \
  --app-state-root /tmp/spec111-deployment-workflow-state-20260714-retry2
```

Verifier command:

```bash
python3 tools/spec111/run_us4_workflow_gate.py \
  --workflow-summary results/spec111-deployment-workflow-smoke-20260714-retry2/deployment-workflow-summary.json \
  --user-log results/spec111-deployment-workflow-smoke-20260714-retry2/llm-pipeline-user.log \
  --controller-log results/spec111-deployment-workflow-smoke-20260714-retry2/controller.log \
  --provider-log results/spec111-deployment-workflow-smoke-20260714-retry2/stage0-provider.log \
  --provider-log results/spec111-deployment-workflow-smoke-20260714-retry2/stage1-provider.log \
  --provider-log results/spec111-deployment-workflow-smoke-20260714-retry2/stage2-provider.log
```

Verified identities/results:

- initial revision: `sha256:56f658b57fea7a085d660591868d832fb3c6348e5a594d6c43eba9f20f6314f4`;
- rollback revision: `sha256:19b0be638acd8dd4253e1cf6bab92a61603351da701075dc99b38384e74714bc`;
- rollback lifecycle epoch: `2`;
- unique durable/wire request bindings: `22`;
- measured fake distributed request: `204.79 ms`;
- terminal deployment state after restart: `DELETED`;
- private Provider evidence keys were temporary `/tmp` inputs, mode `0600`,
  removed by the harness, and are absent from the result directory.

The machine-readable verifier output is
`evidence/us4-deployment-workflow-gate-v2.json`. This gate proves MiniNDN
lifecycle/network correctness only. OCI/SIF, iTiger, real GPU and Qwen
performance remain deferred.
