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

## Wan2.1 prompt and tensor metadata

`wan2.1-t2v-14b-metadata.json` pins the Wan release's DiT shard index and two tokenizer
files. Resolve each listed file below
`https://huggingface.co/<repository>/resolve/<revision>/` into an explicit host asset
root and check the recorded size and SHA-256. The native Requester tokenizer uses
`tokenizer.json`. The independent oracle uses `spiece.model`. For the six
`safetensors_header_ranges`, request only each listed inclusive byte range. Require HTTP
206 and a matching `Content-Range` (including `full_bytes`) before consuming the response.
Verify the header SHA-256. The intervals end at the SafeTensors JSON header and exclude
tensor payloads. The same recipe pins UMT5 and VAE tensor descriptors. With the checkpoint
tool environment, run `tests/fixtures/wan-video/prepare-wan-pytorch-metadata.py --output
"$asset_root"` to recover them through bounded HTTP ranges and PyTorch meta tensors.
For the native metadata selector, copy the pinned recipe to `"$asset_root/recipe.json"` and
set `NDNSF_DI_WAN_CHECKPOINT_METADATA_ROOT="$asset_root"` before selecting
`WanSourceLayout/Wan21Umt5CheckpointBindsTwoLayersAndRunsSplitEntries`. This check runs numerical
inference on the tiny fixture and only plans the production shape.

For raw positive and negative prompt strings, install the pinned host tools from
`wan2.1-text-tools-py38.txt` in a dedicated Python 3.8 environment. Apply Wan's text
cleaning once, before submitting the prepared config to the native Requester. Keep
both configurations in the same directory so relative paths still resolve.

```sh
"$host_python" Experiments/prepare-wan-prompts.py --input raw.json --output prepared.json
"$host_python" tests/fixtures/wan-video/prepare-wan-token-reference.py \
  --asset-root "$asset_root" --output "$run_root/wan-tokens.json"
NDNSF_DI_WAN_TOKENIZER_FILE="$asset_root/google/umt5-xxl/tokenizer.json" \
NDNSF_DI_WAN_TOKEN_REFERENCE="$run_root/wan-tokens.json" \
  "$build_root/model-contracts" \
  --run_test=WanSourceLayout/WanTokenizerPadsBothPromptsAndKeepsEosWhenTruncating
```

The Wan UMT5 and VAE releases use PyTorch ZIP state dicts. On a Python 3.11 host, install
`wan2.1-checkpoint-tools-py311.txt` with the official PyTorch CPU wheel index. Pass
the SHA-256 of the input archive and a fresh output path. The converter maps the source
on CPU, validates the tensor descriptors, and writes FP32 in 1 MiB windows.

```sh
"$checkpoint_python" Experiments/prepare-wan-checkpoint.py \
  --input "$input_pth" --source-sha256 "$source_sha256" --output "$output_safetensors"
"$checkpoint_python" tests/fixtures/wan-video/prepare-wan-checkpoint-fixture.py \
  --output "$run_root/wan-conversion"
NDNSF_DI_WAN_CONVERTED_CHECKPOINT="$run_root/wan-conversion/fixture.safetensors" \
  "$build_root/model-contracts" --run_test=WanSourceLayout/SafeTensorsCheckpointBindsDisjointWanPatchRanks
```

Compile each FP32 checkpoint to a normal ONNX source catalog with the installed
native tool. Provide a fresh destination within an existing parent directory.

```sh
"$prefix/bin/di-prepare-wan-checkpoint" "$request_json" "$component_output"
```

The request uses schema `ndnsf-di-wan-checkpoint-preparation-v1` with these fields.

| Field | Contents |
| --- | --- |
| `component` | `umt5`, `dit`, or `vae` |
| `shards` | Map of archive labels to `{file, bytes, digest}`. Use `sha256:<hex>` and resolve file paths from the request directory. |
| `index` | The same file descriptor for the verified SafeTensors index. Required for multiple shards. Labels match the index's `weight_map`. |
| `stages` | Ordered `{id, roles}` entries. For a single Provider baseline give one stage with one role. For TP give one stage with two roles. For PP give two stages with one role each. DiT also accepts one serial and one two-role stage in either order. |
| `geometry` | UMT5 requires `text_tokens`, `score_bytes`, and `prefix_layers` for PP. DiT requires `grid` with `frames`, `height`, `width`, and `padded_tokens`, plus `heads`, `text_tokens`, `video_key_tile`, `text_key_tile`, `score_bytes`, `rotary_bytes`, and `prefix_blocks` for staged layouts. The grid counts patch positions. VAE requires latent `height`, `width`, `latent_frames`, `key_tile`, `score_bytes`, `latent_mean`, and `latent_inverse_std`. |
| `catalog` | The maintained ONNX source catalog template with model identity, source and publication names, recipe limits, input contract, and resource limits for the requested roles. |
| `max_exact_bundle_bytes`, `timeout_ms` | Exact activation tiling budget and the offline preparation deadline. |

Use the official DiT SafeTensors index and shards directly. Use the converted
FP32 SafeTensors file for UMT5 and VAE. The native command verifies the checkpoint
digests, compiles the selected layout, checks the catalog against the compiled
source, and atomically writes `catalog.json`, `model.onnx`, and the weight
sidecar. The normal StoredModel preparation path consumes that catalog.
The VAE binder reads the checkpoint output projection to select the three-channel
Wan2.1 decoder or the packed twelve-channel Wan2.2 decoder. The latter validates
its grouped upsample blocks and emits RGB through spatial unpatchify. Each
variant retains the same canonical catalog and external slice publication path.
For generation, set `wan_generation.vae_spatial_stride=16` for the Wan2.2
decoder and `frames_per_second=24` for the 5B profile. Existing generation
configurations default to stride 8. Output shape and Response byte admission
use the selected stride before submitting the first inference request.

## Wan2.2 TI2V 5B CPU experiment inputs

`wan2.2-ti2v-5b-metadata.json` pins the original DiT shards, UMT5, VAE,
tokenizer, VAE normalization and independent VAE source revision. Download the
listed model files from its `repository` at its `revision`, verifying each length
and SHA-256. Convert the VAE with `prepare-wan-checkpoint.py` to the recipe's
`converted_vae.file`; convert UMT5 through the same host tool. Use original FP32
DiT shards directly. These are explicit experiment assets, outside ordinary tests.

Generate one request per component and layout, then invoke the existing native
preparer. The generator only writes configuration; C++ verifies weights, lowers
operators, creates the layout and admits the catalog. `single`, `tp`, `pp`,
`pp-tp` and `tp-pp` share model content identity; their immutable source identities
also bind the stage layout, Exact budget and implementation. Converted UMT5
uses the pinned `converted_umt5` file under the same asset root. Resource fields
are conservative declared ceilings, not measured usage or proof of deployment capacity.

```sh
python3 Experiments/prepare-wan22-checkpoint-request.py \
  --asset-root "$asset_root" --component vae --layout single \
  --frames 25 --width 320 --height 320 \
  --requester-prefix /spec271/wan22/requester --service /spec271/wan22/infer \
  --output "$run_root/vae-request.json"
"$prefix/bin/di-prepare-wan-checkpoint" \
  "$run_root/vae-request.json" "$run_root/vae-single"
```

At 24 fps, these 25 frames play for about 1.04 seconds. For the original
15-second stress workload, prepare the DiT and VAE catalogs at 361 frames and
pass the same geometry to the workload generator. At 361 frames, 320×320 fits
the complete FP32 Response bound and 1280×704 exceeds it.

After a real MiniNDN clip completes, create a reference from the official CPU
models using the inputs recorded at each stage. The output binds that clip
manifest and the workload. The native oracle checks all stages, actual role
placement, NDN delivery, and the RGB24 file with the same FP32 tolerance as the
closed-loop comparison. Keep the closed-loop reference and its original result
using the existing `--reference` invocation.

```sh
"$checkpoint_python" Experiments/prepare-wan22-video-reference.py \
  --asset-root "$asset_root" --workload "$run_root/workload.json" \
  --initial-latent "$run_root/initial.raw" --initial-receipt "$run_root/initial.json" \
  --native-clip "$clip_dir" --output "$run_root/same-input-reference"
"$prefix/bin/di-wan-video-oracle" --deployment-state "$operation_json" \
  --conditioned-reference "$run_root/same-input-reference/reference.json" \
  --workload "$run_root/workload.json" --initial-latent "$run_root/initial.raw"
```

For the short real-weight VAE diagnostic, prepare a separate nine-frame 32×32
source using the same commands with `--frames 9 --width 32 --height 32` and fresh
paths. Fetch `vae_reference.file` from its fixed source repository/revision as
`"$asset_root/vae2_2-reference.py"`. The host reference checks its identity,
strictly loads the original checkpoint, and writes deterministic nonzero input
and FP32 RGB. The installed native oracle executes the production CPU runner and
checks every output value, shape and reference digest. It fails on missing or
incomplete comparisons. Use the pinned checkpoint-tool environment (including
einops); this diagnostic does not run automatically or substitute for full video.

```sh
"$checkpoint_python" Experiments/prepare-wan22-vae-reference.py \
  --asset-root "$asset_root" --model "$run_root/vae-nine/model.onnx" \
  --output "$run_root/vae-reference"
"$prefix/bin/di-wan-checkpoint-oracle" "$run_root/vae-reference/oracle.json"
```

The corresponding real DiT and UMT5 checks use the host environment from
`wan2.2-reference-tools-py311.txt`. Fetch each `network_reference.sources` file
from its fixed repository revision to its `local_file` beneath the asset root;
the driver verifies every source length and SHA before import. Prepare the DiT
catalog with nine frames at 32×32, and UMT5 with its normal 512 input slots.

```sh
"$reference_python" Experiments/prepare-wan22-network-reference.py \
  --component dit --asset-root "$asset_root" \
  --model "$run_root/dit-nine/model.onnx" --output "$run_root/dit-reference"
"$prefix/bin/di-wan-checkpoint-oracle" "$run_root/dit-reference/oracle.json"
```

Use `--component umt5` with the UMT5 catalog for the encoder check. This runs
the original encoder on controlled token IDs, independently of tokenization.
The DiT host reference replaces its CUDA-only attention call with FP32 PyTorch
SDPA MATH, preserving key lengths and masking. The unused CUDA wrapper class is
omitted when importing T5; encoder arithmetic remains unchanged. This qualifies
the declared CPU FP32 reference, not official BF16 CUDA equivalence.

The native oracle is an ordinary installed DI diagnostic from `examples/`,
available without enabling the test targets. Inputs support `float32`, `int64`
and `bool`; omitted dtype retains the earlier float32 format. Input and reference
sizes and digests are checked before creating a model session. Its
`factory_create_s` and `execute_s` fields separate factory creation from execution.
These explicit real-weight experiments are outside the ordinary test profiles.

For a real-weight MiniNDN run, bind three prepared catalogs explicitly. Each
catalog must carry the native `preparation_facts` and match the requester
namespace. The host generator does not recompute plans or candidate identities.

```sh
python3 Experiments/prepare-wan22-video-workload.py \
  --asset-root "$asset_root" --umt5-catalog "$umt5_catalog" \
  --dit-catalog "$dit_catalog" --vae-catalog "$vae_catalog" \
  --frames 25 --width 320 --height 320 --steps 50 \
  --namespace /spec271/wan22 --mode single --output "$workload_json"
python3 tests/minindn/prepare-wan-video-run.py \
  --prefix "$prefix" --checkpoint-workload "$workload_json" \
  --namespace /spec271/wan22 --mode single --exact-packet-disk-mib 131072 \
  --tensor-segment-bytes 7600 --output "$run_root"
```

The Spec271 CPU application workload is 25 frames at 24 fps, 320×320,
50 UniPC steps, shift/guidance 5 and seed 0. Its prompt is
`NDN distributed inference`. Prepare DiT and VAE with the matching
`--frames 25 --height 320 --width 320` options. The CLI's 361-frame default
remains available for long-sequence stress experiments.
Use separately prepared 9-frame 32×32 catalogs and explicit
`--frames 9 --width 32 --height 32 --steps 1` for readiness. This short run does
not qualify the final video. Prepare all components for the requested layout;
the native Requester validates geometry and placement before execution.
The launcher keeps shared graph/weight files read-only, writes secondary
configs under the run root and supports its existing `--warm-from` flow.
Distributed checkpoint configurations use the existing 16,384-segment
request limit to admit large VAE edges.
Its large cache limits are conservative accounting ceilings, not RAM allocation.
Use the installed `ndnsf-di cluster check/up/status/down` entry points and retain
the actual configuration, artifact ledger, outcomes and cleanup receipts.
Prepare and run the private work root as the same user; MiniNDN requires root
for network namespaces, and the launcher checks ownership of its operation files.

For a multi-day CPU stress experiment, explicitly set the generated deployment's
`requester.wan_generation.timeout_ms` to `259200000` before freezing its identity,
and pass `--timeout 259200` to `cluster up`. The launcher retains its shorter
default. This finite 72-hour total budget applies to MiniNDN inference runs;
individual requests, grants and assembly keep their independent limits. The
budget is not an estimate of qualified end-to-end performance.

After the installed C++ oracle verifies the completed clip, a host FFmpeg can
package its RGB24 frames as a playable MP4. Keep the raw frames and manifest as
the numerical authority: H.264 with YUV420 conversion is a presentation copy.
Set `wan_width`, `wan_height` and `wan_frames` from the manifest's `width`,
`height` and `frames` fields. Calculate the shell variable
`wan_fps = time_base[1] / (time_base[0] * ticks_per_frame)`. Do not infer them
from the filename. The CPU application workload requires 320×320, 25 frames and 24 fps;
the tiny regression has its own geometry and rate.

```sh
"$wan_ffmpeg" -nostdin -hide_banner -n -f rawvideo -pixel_format rgb24 \
  -video_size "${wan_width}x${wan_height}" -framerate "$wan_fps" \
  -i "$wan_clip/frames.rgb24" -frames:v "$wan_frames" -an \
  -c:v libx264 -threads 2 -preset fast -crf 18 -pix_fmt yuv420p \
  -movflags +faststart "$wan_output.partial.mp4"
"$wan_ffprobe" -v error -count_frames -show_streams -show_frames \
  -select_streams v:0 -of json "$wan_output.partial.mp4"
```

Check the source length and SHA against the manifest before encoding. Require
H.264/YUV420, the declared dimensions, exactly `wan_frames` decoded frames,
`avg_frame_rate == wan_fps`, and
`stream.duration_ts * stream.time_base == wan_frames / wan_fps`. Use rational
arithmetic rather than the rounded decimal `duration` string. For every
display frame `i`, require `best_effort_timestamp * stream.time_base == i / wan_fps`
using rational arithmetic. Publish the final filename without replacing an
existing file only after these checks. Preserve the command, tool identity,
source/output hashes and probe result with the existing experiment receipt.

The presentation path was checked with Ubuntu `ffmpeg`, `libavdevice58` and
`libavresample4` version `7:4.2.7-0ubuntu0.1`, downloaded with `apt-get download`,
verified against the package index SHA-256 and extracted with `dpkg-deb -x` to an
isolated host tool directory. The other matching FFmpeg libraries were already
installed. No system libraries or inference dependencies were changed. If using
this isolated layout, set `LD_LIBRARY_PATH` to its `usr/lib/x86_64-linux-gnu` only
for the FFmpeg/ffprobe children. A successful tiny packaging check does not
qualify a real checkpoint run.
