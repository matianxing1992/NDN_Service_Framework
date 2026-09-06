# Quickstart: Spec 138

Commands become authoritative only after T001 audit and T002 focused tests pass.

```bash
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/138-svs-worker-necessity --strict

python3 tests/python/test_spec138_svs_worker_necessity.py

sudo -E python3 Experiments/NDN_SVS_Worker_Necessity_Minindn.py \
  --campaign results/spec138-svs-worker-necessity/<campaign-id> \
  --preflight

sudo -E python3 Experiments/NDN_SVS_Worker_Necessity_Minindn.py \
  --campaign results/spec138-svs-worker-necessity/<campaign-id> \
  --calibrate-and-qualify

sudo -E python3 Experiments/NDN_SVS_Worker_Necessity_Minindn.py \
  --campaign results/spec138-svs-worker-necessity/<campaign-id> \
  --seal

sudo -E python3 Experiments/NDN_SVS_Worker_Necessity_Minindn.py \
  --campaign results/spec138-svs-worker-necessity/<campaign-id> \
  --run-formal

python3 Experiments/analyze_svs_worker_necessity.py \
  --campaign results/spec138-svs-worker-necessity/<campaign-id> \
  --report specs/138-svs-worker-necessity/evidence/worker-necessity-report.md
```

Before `--run-formal`, inspect the manifest: it must contain one binary hash,
one selected rate, exactly six cells, and `automaticRetry=false`.
