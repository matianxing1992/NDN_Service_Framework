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

## 3a. Reproduce the four-provider local Y-B result

The following is the exact local composition used for the 2026-09-16 result. A
SIF by itself is not the complete experiment input: transfer the immutable SIF,
the matching read-only application bundle, the Y-B case bundle (model, key maps
and catalogue), and the candidate-bound profile together. Keep model and private
test keys in approved private storage; they are not Git artifacts.

```text
spec186-r86-reproduction/
  spec186-local-r86.sif
  spec186-app-bundle-r86-worker/       # read-only directory
  spec186-yolo-minindn-normal-r86-worker.json
  case/Y-B/                            # supplied case bundle
```

Verify the immutable inputs before running. The values below are the expected
ones for this result; a different value is a new candidate and must receive a
new run ID.

```bash
sha256sum spec186-local-r86.sif
# 089a4bc942db5fcc9d01ba2bf62b01ae1836416a232f0806a61931f26d946547

# Compute the application tree digest with the repository candidate helper.
python3 - <<'PY'
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location(
    'spec186_candidate', 'Experiments/TigerCluster/runtime/spec186_candidate.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
print(module.tree_digest(Path('spec186-app-bundle-r86-worker')))
# 92934b89e842483a12b6669cf935b62a1ec8b1750d1093f097ccc4cbcbc58f79
PY
```

Use the supplied r86 profile, or make a path-adjusted copy while preserving all
declared digests. Do not use the older tracked profile that points to the r6
bundle. Generate and validate the candidate without pre-creating its run root:

```bash
PROFILE=/path/to/spec186-yolo-minindn-normal-r86-worker.json
RUN_ID=spec186-yolo-local-normal-r86-replay
RUN_ROOT="$PWD/Experiments/TigerCluster/results/$RUN_ID"
CANDIDATE="$PWD/$RUN_ID.candidate.json"

test ! -e "$RUN_ROOT"
python3 Experiments/TigerCluster/jobs/spec184/submit.py prepare \
  --profile "$PROFILE" --run-id "$RUN_ID" \
  --candidate-output "$CANDIDATE" \
  --output "$PWD/$RUN_ID.prepare.json"
python3 Experiments/TigerCluster/jobs/spec184/submit.py check \
  --profile "$PROFILE" --candidate "$CANDIDATE"
sudo -E python3 Experiments/TigerCluster/jobs/spec184/submit.py local \
  --profile "$PROFILE" --candidate "$CANDIDATE" \
  --run-id "$RUN_ID" --run-root "$RUN_ROOT"
```

The `local` command must run as root because MiniNDN/Mininet creates network
namespaces. It creates the writable per-run HOME, NFD sockets and identity state;
do not use a shared host `.ndn` directory or a volatile `/tmp` state root. The
application bundle is mounted read-only and the SIF is never modified.

For the reference candidate, the expected application markers are ACK count 4,
four-role Selection, four-provider execution start and:

```text
YOLO_ACK_DRIVEN_RESULT status=true payload_bytes=1267
```

`evidence/yolo-numerical.json` should report `matched=true`, shape `[1,50,6]`
and `maxAbsError=0.0005340576171875`. The three partition providers report
`realCompute=true` with `onnxruntime-cpu`; `Merge` reports completed
`native-yolo-postprocess`. Always inspect the lifecycle and every
`provider-*.log`, not only the final response. The recorded r86 harness ended
with forced cleanup (`exitCode=-15`), so reproducing the numerical response is
an application-execution result, not a clean qualification PASS until teardown
also exits normally.

For local SIF work, use the profile's explicit `/usr/local/bin/apptainer`
1.5.3 runtime. Tiger profiles use the project-owned
`/home/tma1/.local/bin/apptainer-1.5.3` on allocated compute
nodes. The Tiger login node's `/usr/bin/apptainer` is 1.3.4 and is metadata
only; it never executes a SIF and is not a fallback.

Before the Tiger submit, complete the local promotion sequence: build the sealed
SIF with `/usr/local/bin/apptainer` 1.5.3, run the preflight/import/loader checks and
local CPU smoke, record its byte SHA-256, then upload that exact file. On the login
node check only path/quota metadata; on the allocated compute node verify the uploaded
SHA with `/home/tma1/.local/bin/apptainer-1.5.3` before running. A compute-side rebuild
is an exception that needs a new source-sealed candidate and a preserved local failure.

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
