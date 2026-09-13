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

## 2. Prepare And Check

```bash
PROFILE=Experiments/TigerCluster/profiles/spec184-yolo-minindn-normal.json
RUN_ID=spec186-y-a-<date>
RUN_ROOT=Experiments/TigerCluster/results/$RUN_ID
CANDIDATE=/tmp/$RUN_ID.candidate.json

python3 Experiments/TigerCluster/jobs/spec184/submit.py prepare \
  --profile "$PROFILE" --run-id "$RUN_ID" \
  --candidate-output "$CANDIDATE" --output "$RUN_ROOT/prepare.json"

python3 Experiments/TigerCluster/jobs/spec184/submit.py check \
  --profile "$PROFILE" --candidate "$CANDIDATE" \
  > "$RUN_ROOT/check.json"
```

`check` and `prepare` MUST be side-effect-free with respect to SSH, staging, Slurm and
remote storage. The candidate manifest is kept outside Git; the prepare/check receipts
are retained under the declared run root.

## 3. Local MiniNDN

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --help
sudo -E python3 Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py --help
python3 Experiments/TigerCluster/jobs/spec184/submit.py local \
  --profile "$PROFILE" --candidate "$CANDIDATE" --run-id "$RUN_ID" \
  --run-root "$RUN_ROOT/local"
```

The Qwen case is accepted only when the model descriptor identifies actual Qwen3-0.6B
weights and a compatible backend. A tiny fixture produces `SMOKE_ONLY`, not model PASS.

For local SIF work, use the profile's explicit `/usr/local/bin/apptainer`
1.5.3 runtime. Tiger profiles use `/usr/bin/apptainer` on allocated compute
nodes reporting 1.5.3. The Tiger login node's 1.3.4 package is metadata only;
it never executes a SIF and is not a fallback.

## 4. TigerCluster

```bash
python3 Experiments/TigerCluster/jobs/spec184/submit.py submit \
  --profile "$PROFILE" --candidate "$CANDIDATE" --run-id "$RUN_ID" \
  --run-root "$RUN_ROOT/tiger"
python3 Experiments/TigerCluster/jobs/spec184/submit.py collect \
  --candidate-digest "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["candidateDigest"])' "$CANDIDATE")" \
  "$RUN_ROOT/tiger/receipt.json"
```

The submitter MUST verify the immutable candidate and effective profile before transport.
The collector MUST require fresh protocol, numerical, backend/GPU, process-exit and
cleanup evidence before recording PASS.
