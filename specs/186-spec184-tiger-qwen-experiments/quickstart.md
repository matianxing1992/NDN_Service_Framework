# Spec186 Quickstart (Planned)

The commands below describe the implementation contract. They are not evidence until a
fresh Spec186 candidate and run ID produce retained receipts.

## 1. Resolve Baseline

```bash
git rev-parse HEAD
git status --short
git switch -c SPEC184Experiments 575b43cc93bbed29932303caf3d09974f1585af7
```

If the object is absent, obtain it from a clean clone or an approved reachable remote
ref first. The command MUST report the exact 40-hex SHA before any build.

## 2. Check And Prepare

```bash
python3 Experiments/TigerCluster/jobs/spec184/submit.py check \
  --profile Experiments/TigerCluster/profiles/spec184-yolo-two-node.json \
  --run-id <run-id> --output Experiments/TigerCluster/results/<run-id>

python3 Experiments/TigerCluster/jobs/spec184/submit.py prepare \
  --profile <profile.json> --run-id <run-id> --output <run-dir>
```

`check` and `prepare` MUST be side-effect-free with respect to SSH, staging, Slurm and
remote storage. They write only the declared local run receipt.

## 3. Local MiniNDN

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --help
sudo -E python3 Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py --help
python3 Experiments/TigerCluster/jobs/spec184/submit.py local \
  --profile <prepared-profile.json> --run-id <run-id> --output <run-dir>
```

The Qwen case is accepted only when the model descriptor identifies actual Qwen3-0.6B
weights and a compatible backend. A tiny fixture produces `SMOKE_ONLY`, not model PASS.

## 4. TigerCluster

```bash
python3 Experiments/TigerCluster/jobs/spec184/submit.py submit \
  --profile <prepared-profile.json> --run-id <run-id> --output <run-dir>
python3 Experiments/TigerCluster/jobs/spec184/submit.py collect \
  --profile <prepared-profile.json> --run-id <run-id> --output <run-dir>
```

The submitter MUST verify the immutable candidate and effective profile before transport.
The collector MUST require fresh protocol, numerical, backend/GPU, process-exit and
cleanup evidence before recording PASS.
