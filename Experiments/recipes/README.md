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

## Wired Repo original-packet restart

Use the installed `ndnsf-repo-file-benchmark` target and Ops wheel. Prepare a private
`qwen06b-wired-star-tp-cpu` deployment with the installed preparation command. This provides
the Requester, Repo and control identities and `Experiments/Topology/spec270-wired-star.conf`
(1 Gbit/s and 0.25 ms per link). The Repo benchmark launches the preparation projection of the
deployment and performs file I/O. Generate the input in the private work root:

```sh
repo_file_bytes=${repo_file_bytes:-500000000}
input_sha256=$(python3 - "$work_root/repo-input.bin" "$repo_file_bytes" <<'PY'
import hashlib
import sys

total = int(sys.argv[2])
assert total in (500_000_000, 20_000_000_000)
block_size = 1 << 20
digest = hashlib.sha256()
with open(sys.argv[1], "xb") as file:
    for index, offset in enumerate(range(0, total, block_size)):
        block = hashlib.shake_256(b"NDNSF Spec 271 recovery fixture v1" +
                                  index.to_bytes(8, "big")).digest(min(block_size, total - offset))
        file.write(block)
        digest.update(block)
if total == 500_000_000:
    assert digest.hexdigest() == "78368e90250b5865c0e7ca1fa7d922bcea90ad0ae85a65f30d10610733abbb6d"
print(digest.hexdigest())
PY
)
```

Generate two configs with `ops.deployment_config.preparation_deployment`. Give them distinct
`deployment_id` values and keep the same `storage.repo_root`, topology and credentials.
Place the Repo on `provider1` so that its traffic to and from the Requester crosses the router.
Use the same `--authentication` and, when choosing an explicit Producer certificate, the same
`--signing-certificate` in both phases. Put the reference in the private work root.

```sh
/usr/bin/python3 -I - "$work_root/deployment.json" <<'PY'
import json
from pathlib import Path
import sys

from ndnsf_di_launcher import _bind_package_root
_bind_package_root()
from ndnsf_distributed_inference.ops.deployment_config import preparation_deployment

source = Path(sys.argv[1]).resolve(strict=True)
for phase in ("write", "read"):
    config = preparation_deployment(json.loads(source.read_bytes()))
    config["deployment_id"] += "-" + phase
    next(actor for actor in config["actors"] if actor["kind"] == "repo")["node"] = "provider1"
    with source.with_name("repo-" + phase + ".json").open("x") as output:
        json.dump(config, output, sort_keys=True)
PY
write_config="$work_root/repo-write.json"
read_config="$work_root/repo-read.json"
```

```sh
sudo -n /usr/local/bin/ndnsf-di cluster benchmark-repo --config "$write_config" \
  --authentication manifest --phase write --file-bytes "$repo_file_bytes" \
  --reference "$work_root/reference.json" \
  --source "$work_root/repo-input.bin" --expected-sha256 "$input_sha256"
sudo -n /usr/local/bin/ndnsf-di cluster benchmark-repo --config "$read_config" \
  --authentication manifest --phase read --file-bytes "$repo_file_bytes" \
  --reference "$work_root/reference.json" \
  --expected-sha256 "$input_sha256"
```

The write creates an exclusive reference. The independent read checks each group's original
wire digest and the reconstructed file SHA-256. Record the installed binary and wheel identities,
topology and config digests, both Repo PIDs, complete native records, and nonforced cleanup.
Use one write and read as a restart check. A throughput comparison additionally needs repeated
matched trials and bare NFD replay of the same original wire in each applicable direction.

For the local 20 GB test, set `repo_file_bytes=20000000000` before generating the input.
Reserve space for the dense input and its original-wire Repo store. Set
`storage.repo_root` in both configs to a new private directory for this trial.
Allow `--timeout 1800` for each phase. Use ordered durable group completions and verified
retrieval records to report throughput over cumulative payload intervals. Measure from each
native phase start and retain the process and configuration evidence. After every trial,
confirm that both deployments have stopped and retain their reports before removing the
trial's private Repo store and generated source. Create a fresh private store for any retry.

For the 8,400 B tuned comparison, set the Repo actor's `resources.cpus` to at least five
and give its `parameters` the following native settings. This applies to the new write
and the independent read.

```json
{
  "handler-threads": 5,
  "manifest-fetch-window": 28,
  "manifest-source-window": 40,
  "manifest-handler": "reserved",
  "manifest-fetch-face": "dedicated"
}
```

Use `--manifest-segment-bytes 8400` on both invocations. Add
`--manifest-group-segments 3840 --manifest-insert compact --manifest-write-slots 5` to the
write and `--manifest-read-pipeline quad --manifest-read-total-window 64
--manifest-owner-claims targeted` to the read. These settings apply only when explicitly
selected. For the split-core comparison, pass
`--cpu-placement Experiments/profiles/repo-wired-star-split.json` to both Repo and NFD
commands. The launcher checks the actual native process affinities against this map.
The bare replay uses the first CPU of each endpoint's list for its corresponding thread.
For the memory-spool comparison, set the writer Repo actor's `manifest-spool` parameter
to `bounded-memory`. Authenticated source wires use at most 32 MiB per object and 64 MiB
per Repo process. Groups without a reservation use the default `file` spool.
Run `--timing` separately from
the throughput samples to capture source fetch, spooling and durable confirmation events.

Build and install the Waf target `ndnsf-nfd-wire-benchmark` from the same candidate. After
stopping the Repo, replay a complete group from its filesystem store through the same topology.
Run once per direction with the same CPU placement as the Repo samples. The default uses the host's
unrestricted CPU placement, three 10 s steady samples and windows 16 and 32.

```sh
sudo -n /usr/bin/python3 -I Experiments/run-nfd-wire-benchmark.py \
  --config "$write_config" --reference "$work_root/reference.json" \
  --store "$repo_root/repo.sqlite.store" --direction write --output "$work_root/nfd-write"
sudo -n /usr/bin/python3 -I Experiments/run-nfd-wire-benchmark.py \
  --config "$read_config" --reference "$work_root/reference.json" \
  --store "$repo_root/repo.sqlite.store" --direction read --output "$work_root/nfd-read"
```

The digest and size in the write reference identify the original-wire file. Replay checks each
returned packet byte for byte with NFD caching disabled. The result includes all attempts and
only reports a sustained median for windows whose entire sample set completed without errors.
Use `--samples 1 --seconds 1 --windows 16` for a connectivity check.

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

python3 Experiments/UAV/run_multicamera_tracking_demo.py \
  --source /persistent/models/uav-multicam-tracking-v1 \
  --model /persistent/models/uav-multicam-tracking-v1/3UAVs_YOLOv8n.pt \
  --license Apache-2.0 --output-dir "$RUN_DIR" --prepare --preflight --headless
```

`--model` must be passed: the demo's `DEFAULT_MODEL` constant names `3UAVs.pt`, while the
upstream artifact is `3UAVs_YOLOv8n.pt`, so a default run reports a missing checkpoint even
with the assets present. The recipe records this as `knownGap`.

These clips carry no synchronized GPS/IMU or camera calibration truth. A run over them keeps
`acquisitionKind=simulated` and `accuracyQualification=unvalidated`; it exercises the tracking
lifecycle, not detection or association accuracy.
