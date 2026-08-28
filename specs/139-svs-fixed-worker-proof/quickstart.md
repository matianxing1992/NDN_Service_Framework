# Quickstart

```bash
python3 tests/python/test_spec139_svs_fixed_worker_proof.py

sudo -E python3 Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py \
  --campaign results/spec139-svs-fixed-worker-proof/<id> --qualify

sudo -E python3 Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py \
  --campaign results/spec139-svs-fixed-worker-proof/<id> --seal

sudo -E python3 Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py \
  --campaign results/spec139-svs-fixed-worker-proof/<id> --run-formal

python3 Experiments/analyze_svs_fixed_worker_proof.py \
  --campaign results/spec139-svs-fixed-worker-proof/<id> \
  --report specs/139-svs-fixed-worker-proof/evidence/fixed-worker-report.md
```
