# US2 Policy and External Optimizer Regression Gate

Date: 2026-07-14  
Verdict: **PASS**

The focused engine, policy, negative-boundary, recovery, observer, adapter,
runtime-aware planner/campaign and lineage suite ran 86 tests in 1.612 seconds:
86 passed, zero failed, zero errored and zero skipped.

The standalone optimizer wheel SHA-256 is
`780bdab433438a9ebad9d4d51763b5a391042a624a5267df6545a97b59a8f0e4`.
It imports only public Core/SDK contracts, supplies all ten policies, an
independent Runner adapter and an optional observer, and passed direct,
partial-default and exact-allowlist discovery tests.

```bash
sudo -n env -u NDN_LOG \
  PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper:/tmp/spec111-ext-site" \
  python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --core-trace --tracer-deterministic-runner \
  --enable-execution-leases --requests 1 --concurrency 1 \
  --skip-provider-pair-telemetry-probe \
  --spec111-optimizer-module ndnsf_di_external_optimizer \
  --out results/spec111-us2-external-optimizer-4d695ce8b7ff-bb32fe4c
```

Observed `SUCCESS`. The external assignment policy emitted four role mappings
that the native MiniNDN selector consumed. `summary.json` SHA-256 is
`24d1d0bf86c38f5d4b39ac27d13049411b2f2cec42cbaa676120dd31279069cd`.
Its deterministic runner is deliberately labelled `invalid-evidence`: this
proves protocol/policy integration, not real-model or hardware performance.
