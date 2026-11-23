# Offline model preparation

Ordinary installation does not download model assets. The explicit Qwen CPU recipe is
`qwen3-0.6b-int4.json`. It binds the public source revision, every downloaded file, the separate
base-model Apache license, conversion tool versions, intermediate output, canonical graph,
external weights and tokenizer files. `tokenizer.json` contains the vocabulary and merges;
the standalone `vocab.json` and `merges.txt` are not inputs to this recipe.

Prepare in a dedicated host Python environment, outside runtime containers. For the supported
Python 3.8 tool environment:

```sh
python3 -m venv /path/to/model-tools
/path/to/model-tools/bin/python -m pip install --only-binary=:all: -r Experiments/recipes/model-tools-py38.txt
/path/to/model-tools/bin/python Experiments/prepare-model-assets.py \
  --recipe qwen3-0.6b-int4 --asset-root /persistent/models/qwen3-0.6b-int4-v1 --allow-download
```

The parent of `--asset-root` must already exist. The preparer needs at least 4 GiB free for
source and conversion outputs. It retains roughly 1.8 GiB of immutable model inputs and outputs,
uses one exclusive purpose lock, rejects an existing incomplete directory, and removes its own
large partial output after failure or interruption. Choose persistent model storage outside the
checkout and temporary directories. Reusing a SEALED directory rechecks all committed files and
does not download or reconvert. Keep the same root across retries; do not allocate timestamped
model copies.

For an offline source mirror, use `--source-dir DIR --offline`. `DIR` must contain the exact
paths in `sourceFiles` and the base-model `LICENSE` at its root. No other files are copied.
`--offline --allow-download` is rejected. Missing files, LFS pointers, wrong revision, changed
weights, insufficient space or failed conversion cannot produce an admissible SEALED manifest.

The asset contains two distinct representations of the same upstream model. `sourceModel`
is the original inline `onnx/model_q4.onnx` for automatic ONNX inspection. `canonicalGraph` and
`canonicalWeights` are the normalized external-data representation used by TP diagnostics.
Their identities must not be substituted for one another. Historical diagnostic token fixtures
live under `tests/fixtures/qwen-tp-history`; the fresh deployment workload has its own profile.

Run the declared model checks with the actual manifest file:

```sh
python3 tests/run-checks.py --profile model \
  --asset-manifest /persistent/models/qwen3-0.6b-int4-v1/asset-manifest.json
```

The runner validates the maintained recipe and all file commitments, then passes exact role
paths to the selected native checks. A directory containing a similarly named ONNX file is
insufficient. An invalid asset is a prerequisite failure, never a skipped PASS.

Preparation verifies identities, token/KV IO, external-data ranges and INT4 block attributes.
This upstream ORT export contains fused operators outside the generic ONNX checker registry,
including `SimplifiedLayerNormalization`. Native ORT CPU execution and independent numerical
oracles qualify inference separately; the preparation receipt does not claim those results.

## UAV multi-camera tracking assets

`uav-multicam-tracking.json` declares the detector checkpoint and three view clips that
`Experiments/UAV/run_multicamera_tracking_demo.py` consumes. It records both HuggingFace
revisions, every file's byte count and SHA-256 (taken from the repository's LFS object ids),
and the LICENSE role.

Download and verify them into one directory, then pass it explicitly:

```bash
python3 - <<'PY'
import json, urllib.request, hashlib, pathlib
recipe = json.load(open("Experiments/recipes/uav-multicam-tracking.json"))
root = pathlib.Path("/persistent/models/uav-multicam-tracking-v1"); root.mkdir(parents=True, exist_ok=True)
for source in recipe["sources"]:
    target = root / source["file"]
    if not target.is_file():
        urllib.request.urlretrieve(source["url"], target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    assert digest == source["sha256"], (source["file"], digest)
print("verified", len(recipe["sources"]), "files in", root)
PY

python3 Experiments/UAV/prepare_vehicle_detector.py \
  --checkpoint /persistent/models/uav-multicam-tracking-v1/3UAVs_YOLOv8n.pt \
  --checkpoint-sha256 c51091c489af103ab641c3db0580231b1ed622bfa1f95694ef0fa59607ff1117 \
  --output /persistent/models/uav-multicam-tracking-v1/3UAVs_YOLOv8n.onnx

python3 Experiments/UAV/run_multicamera_tracking_demo.py \
  --source /persistent/models/uav-multicam-tracking-v1 \
  --model /persistent/models/uav-multicam-tracking-v1/3UAVs_YOLOv8n.onnx \
  --license Apache-2.0 --output-dir "$RUN_DIR" --prepare --preflight --headless
```

The default model path is `3UAVs_YOLOv8n.onnx` under `NDNSF_SPEC191_ASSET_ROOT`.
Keep the checkpoint for reproducible preparation and the ONNX digest for deployment evidence.

These clips carry no synchronized GPS/IMU or camera calibration truth. A run over them keeps
`acquisitionKind=simulated` and `accuracyQualification=unvalidated`; it exercises the tracking
lifecycle, not detection or association accuracy.
