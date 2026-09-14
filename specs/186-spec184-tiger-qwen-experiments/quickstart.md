# Spec186 Quickstart (Direct MiniNDN + TigerCluster path)

The commands below describe the implementation contract. They are not evidence until a
fresh Spec186 candidate and run ID produce retained receipts.

The direct result is a real YOLO run in MiniNDN followed by the same base SIF plus
read-only application bundle on TigerCluster (single-node GPU, two-node normal/negative,
then independent reuse). `check` and `prepare`, static tests, and local loader probes
are fail-closed prerequisites; they are not substitutes for those runtime rows. Qwen3-
0.6B is an auxiliary CPU path and cannot close a missing YOLO result.

## 1. Resolve Baseline

```bash
git rev-parse HEAD
git status --short
git switch -c SPEC184Experiments 575b43cc93bbed29932303caf3d09974f1585af7
```

If the object is absent, obtain it from a clean clone or an approved reachable remote
ref first. The command MUST report the exact 40-hex SHA before any build.
After the authorized Spec186 repairs, the candidate may pin a descendant commit; verify
`git merge-base --is-ancestor 575b43cc93bbed29932303caf3d09974f1585af7 HEAD` and record
the candidate commit and source seal instead of silently using an unrelated tip.

## 2. Prepare And Check

```bash
PROFILE=Experiments/TigerCluster/profiles/spec184-yolo-minindn-atomic.json  # Y-A
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

Repeat the same `prepare → check → local` sequence with
`spec184-yolo-minindn-normal.json` (Y-B) and
`spec184-yolo-minindn-negative.json` (Y-N). Each case bundle is immutable and
contains its canonical YOLO26n package, signed catalogue inputs, topology and
offer key maps; do not substitute the old YOLOv8 fixture.

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
1.5.3 runtime. Tiger profiles use the project-owned
`/project/tma1/ndnsf-di/tools/spec186/apptainer-1.5.3` on allocated compute
nodes. The Tiger login node's `/usr/bin/apptainer` is 1.3.4 and is metadata
only; it never executes a SIF and is not a fallback.

## 4. TigerCluster

```bash
python3 Experiments/TigerCluster/jobs/spec184/submit.py submit \
  --profile "$PROFILE" --candidate "$CANDIDATE" --run-id "$RUN_ID" \
  --run-root "$RUN_ROOT/tiger"
python3 Experiments/TigerCluster/jobs/spec184/submit.py collect \
  --candidate-digest "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["candidateDigest"])' "$CANDIDATE")" \
  "$RUN_ROOT/tiger/receipt.json"
```

For Tiger, use `spec184-yolo-tiger-single-gpu.json` for Y-A, then the two-node
normal/negative/reuse profiles. The rendered job wraps the replay harness in the
declared compute-node Apptainer 1.5.3 executable and image and mounts the same read-only
application bundle and case bundle.

The submitter MUST verify the immutable candidate and effective profile before transport.
The collector MUST require fresh protocol, numerical, backend/GPU, process-exit and
cleanup evidence before recording PASS.

## 5. SIF pre-build gate

Before any full native build, render the locked definition and run the cheap
cross-check. It verifies the source archive and Waf template, NumPy's private
wheel libraries and RPATH destination, and imports NumPy from the exact base SIF
inside a temporary writable overlay:

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-development-sif.py \
  --definition /absolute/path/to/rendered-runtime.def \
  --apptainer /usr/local/bin/apptainer \
  --base-sif /absolute/path/to/base.sif
```

`SPEC186_PREFLIGHT_PASS` is only a build-input gate. If it fails, preserve the
diagnostic and fix the definition, sealed input, or base composition before
compiling. Do not treat it as MiniNDN or Tiger evidence.
