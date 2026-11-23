# Spec191 UAV tracking experiment

## Indoor person tracking on CPU

The same `run_multicamera_tracking_demo.py` entrypoint supports three indoor
video sources and one compute node running YOLO, ByteTrack and pretrained
OSNet. Native C++ assigns the global person IDs; the Ground Station renders
its validated results and frames retrieved from the source nodes. Replay with
fixed cameras simulates hovering UAVs for indoor tracking tests.

### Native ONNX replay

Provide the detector and OSNet ONNX files and put their SHA-256 digests in the
person profile. For YOLO26n, select profile schema
`uav-person-tracking-profile-v2` and set `detectorPreprocessId` to
`yolo-default640-stock-box-v1`. The detector needs dynamic image height and
width and either an end-to-end `[1,300,6]` output or a prepared dynamic raw
head with a fixed class axis. Raw graphs carry the `ndnsf.uav.class-names.v1` class index; the native
session takes argmax before selecting person detections and applying the
person limit. The OSNet graph uses the profile's existing 512-dimensional GAP
embedding contract. Crops use rounded, clamped integer boxes and the stock
PIL uint8 bilinear transform; crops smaller than 16 by 32 pixels retain their
local track without an appearance embedding.

Pass both ONNX files to the existing launcher. It selects the installed main
GS and Drone applications and generates the computation manifest with the
same Drone executable that will run Compute. See the source manifest fields
below. Keep the run duration equal to the source manifest duration.

```bash
python3 Experiments/UAV/run_multicamera_tracking_demo.py \
  --run --headless --run-seconds 2 \
  --model "$DETECTOR_ONNX" --reid-model "$REID_ONNX" \
  --tracking-profile "$PERSON_PROFILE" --source-manifest "$SOURCE_MANIFEST" \
  --output-dir "$RUN_DIRECTORY"
```

### Prepare person checkpoints for native replay

Checkpoints are offline preparation inputs. Use the existing detector exporter
with `--tracking-mode person`; it preserves either the raw or end-to-end head
and checks two rectangular input shapes before publishing the ONNX file.
End-to-end graphs require person at class index zero; raw graphs use the
unique person index from their class metadata.
The default vehicle mode continues to require a raw head. Preparation versions
and the fixed Torchreid source are recorded in the
[shared input recipe](../recipes/uav-multicam-tracking.json). Both exporters run
on the preparation host.

```bash
python Experiments/UAV/prepare_vehicle_detector.py --tracking-mode person \
  --checkpoint "$DETECTOR_CHECKPOINT" --checkpoint-sha256 "$DETECTOR_SHA256" \
  --output "$DETECTOR_ONNX" > "$DETECTOR_EXPORT_LOG"
python tools/person-reid/export_osnet_reid.py \
  --checkpoint "$REID_CHECKPOINT" --dataset "$REID_DATASET" \
  --output "$REID_ONNX" --receipt "$REID_EXPORT_RECEIPT"
python NDNSF-UAV-APP/tracking/person_profile.py \
  --template "$PERSON_PROFILE_TEMPLATE" --detector "$DETECTOR_ONNX" \
  --reid "$REID_ONNX" --output "$PERSON_PROFILE"
```

The operator template must explicitly select schema
`uav-person-tracking-profile-v2` and detector preprocessing
`yolo-default640-stock-box-v1`, with the existing GAP ReID preprocessing.
The last command binds the two actual ONNX digests without changing the template's
policy. Keep the detector export log (its final line is the JSON receipt) and
the ReID receipt; start a new run with its newly generated
computation manifest rather than resuming checkpoint worker state.

### Offline native crop reference

Use the frozen host inference environment for this preparation step. The box
file is a JSON array of four-coordinate observations. The reference uses the
checkpoint and stock Pillow/torchvision transform rather than native resizing:

```bash
python tools/person-reid/make_person_golden.py --stock-crops \
  --image "$REFERENCE_IMAGE" --boxes "$REFERENCE_BOXES_JSON" \
  --reid "$REID_CHECKPOINT" --output "$GOLDEN_DIRECTORY"
python tools/person-reid/export_osnet_reid.py \
  --checkpoint "$REID_CHECKPOINT" --dataset "$REID_DATASET" \
  --output "$REID_ONNX" --receipt "$GOLDEN_DIRECTORY/onnx-export.json"
NDNSF_PERSON_CROP_GOLDEN="$GOLDEN_DIRECTORY" \
  ./build/uav-tracking-provider-tests \
  --run_test=UavTrackingProviderLifecycle/PersonReidMatchesIndependentStockCheckpoint
```

The C++ case requires the prepared files, verifies the export's checkpoint and
ONNX identities, compares every input float exactly and checks all 512 output
features against the independent reference. It also compares native JPEG RGB
bytes when the input is a JPEG. This is a numeric boundary check; the three
sources, cross-window IDs and cancellation still need their runtime checks.

For real indoor footage, the [EPFL laboratory sequences](https://www.epfl.ch/labs/cvlab/data/data-pom-index-php/)
provide four synchronized 25 FPS cameras. Cameras 0, 1 and 2 of the four-person
sequence are suitable source inputs for a small three-view integration probe.
EPFL permits research use and requests citation for published results; retain
this attribution in the source manifest. Its ground-truth files are separate
offline inputs. Source download URLs are
`https://documents.epfl.ch/groups/c/cv/cvlab-pom-video1/www/4p-c{0,1,2}.avi`.
These videos are operator inputs and are not downloaded by the installer.
Pin a checkpoint from the [OSNet author's model repository](https://huggingface.co/kaiyangzhou/osnet)
for offline export and retain its source identity, digest and export receipt.
Model preparation alone does not qualify tracking or cross-camera accuracy.

- Install the native `UavDroneApp`, `UavGroundStationApp` and
  `App_ServiceController` programs and tracking modules through the maintained
  Waf build/install workflow. The runner resolves installed programs, not
  binaries from the build tree. Select `--application-mode main` explicitly;
  the historical `compat` mode uses the separate tracking programs.
- Run the launcher with a host Python environment that can import MiniNDN and
  Mininet. FFmpeg/ffprobe and the MiniNDN system dependencies must be installed.
  Model execution stays in the installed native Compute application; the
  offline preparation tools have their own versions in the shared recipe.
- Provide the prepared detector and OSNet ONNX files. Create an operator-owned
  profile following [the native profile preparation requirements](#prepare-person-checkpoints-for-native-replay),
  including its schema and preprocessing selection, actual ONNX digests and
  CPU device. Set camera relations for the actual setup;
  unverified relations are development assumptions. Do not use the zero-hash
  template as a runnable profile.
- Create the source-only JSON manifest described below. Ground truth belongs
  only to the separate offline evaluator, not the source manifest or
  application inputs.

The source manifest has these required top-level fields:
`schema` (`uav-person-source-manifest-v1`), `dataset` (nonempty `name`, `uri`,
`license`), `commonStartUs`, `durationUs`, and `cameras`. The camera array is
ordered `UAV1`, `UAV2`, `UAV3`. Each entry has exactly `cameraId`, `viewId`,
`videoPath`, `videoDigest` and `videoStartUs`. Paths may be absolute or relative
to the manifest. Use three distinct files, contents and view IDs; each video
digest is `sha256:` plus its SHA-256. `videoStartUs` maps each video's media
time to `commonStartUs`. The interval must be covered by all three videos and
contain whole 0.5-second windows. A three-window probe needs `durationUs` of
1500000; a 120-window run needs 60000000. Declaring a time mapping does not
establish physical synchronization or complete cross-camera annotations.

Add the optional top-level `datasetOrigin` as `real-captured` or
`synthetic-rendered`, and use the same value in the separate evaluation
manifest. Preparation receipts, run receipts and metrics retain this field.
Older manifests without it remain usable for development and offline scoring;
qualification requires an explicit, matching origin. Rendered video still uses
the actual models and native execution path. The separate run-receipt
`synthetic` flag describes mock execution, and must remain false for qualification.

After an independent data review, the offline evaluator accepts an explicit local
`--data-admission FILE` together with `--run-receipt FILE`. The admission record
binds the reviewed source, evaluator and profile digests, dataset origin and
preparation audit. It is local review evidence, not network authorization. The
evaluator checks these bindings and the audit digest, then reports its assessment
in `qualificationEvidence` without changing the original runtime receipt. Actual
CPU telemetry and verified installed closure are required to classify unknown
execution as non-mock; an explicit mock remains a mock. All accuracy, completeness,
resource, deadline and cleanup gates still apply. Without this optional record,
the existing unassessed runtime qualification remains unchanged.

The following Bash template runs a three-window CPU probe from the repository
root after replacing the absolute input paths. `DEMO_PYTHONPATH` must include
the installed tracking directory and the directories containing the MiniNDN
and Mininet Python modules. Use a fresh, dedicated output directory. These
are operator command templates, not a claim that these example paths exist.

```bash
LAUNCHER_PYTHON=/absolute/path/to/minindn-control/bin/python
DEMO_PYTHONPATH=/usr/local/libexec/ndnsf-uav/tracking:/path/to/mini-ndn:/path/to/mininet-module-parent
PERSON_PROFILE=/absolute/path/to/person-cpu-profile.json
SOURCE_MANIFEST=/absolute/path/to/three-view-sources.json
DETECTOR_MODEL=/absolute/path/to/prepared-models/person-detector.onnx
REID_MODEL=/absolute/path/to/prepared-models/osnet.onnx
DEMO_RUN_ROOT=/absolute/path/to/new-person-run

person_args=(
  --application-mode main
  --model "$DETECTOR_MODEL"
  --tracking-profile "$PERSON_PROFILE"
  --source-manifest "$SOURCE_MANIFEST"
  --reid-model "$REID_MODEL"
  --output-dir "$DEMO_RUN_ROOT"
  --run-seconds 1.5 --window-count 3 --global-deadline-ms 180000
)

env PYTHONPATH="$DEMO_PYTHONPATH" "$LAUNCHER_PYTHON" \
  -m Experiments.UAV.run_multicamera_tracking_demo \
  "${person_args[@]}" --prepare --preflight

sudo env DISPLAY="$DISPLAY" XAUTHORITY="$XAUTHORITY" \
  PYTHONPATH="$DEMO_PYTHONPATH" "$LAUNCHER_PYTHON" \
  -m Experiments.UAV.run_multicamera_tracking_demo \
  "${person_args[@]}" --run
```

For a host without a desktop, add `--headless` to both commands; its records
do not prove that GUI windows were shown. Keep the same inputs and output
root between preparation and its run. After a network attempt, use a new
output root for another attempt; preserve the first attempt's evidence.
`--run-seconds` must equal the manifest interval, and `--window-count` must
equal its 2 Hz window count. Unavailable requested devices and
checkpoint/profile mismatches fail preflight; there is no automatic device
fallback. The run verifies its frozen installed-file hashes, so keep the
installation unchanged during a run. Person mode cannot be combined with legacy motion, custom
observation profiles/adapters, source cutovers or custom trust inputs.

Inspect `manifest.json`, `preflight.json`, `person-run-receipt.json` and
`person-cleanup.json` in the output root. For the GUI three-window probe, check
three committed windows, nine admitted/received frames, actual CPU devices,
complete timing/resources and zero remaining owned processes. A GUI run also
needs nine displayed frames; retain actual screenshots with their
run/mission/window/result-digest bindings. The GS-received source JPEGs and
original result JSON are bound in
`private/ground-station/presentation/window-N/presentation.json`; the renderer
does not obtain its display results from the compute backend's private files.
Global/local badges and association status are native outputs. Source videos
may already contain annotations; such burned-in boxes are not native output.
Unknown geometry keeps speed, course and range unavailable. Replay lag is
shown explicitly and must not be reported as real-time performance.
Headless runs can establish processing, reception and cleanup observations;
GUI display timestamps and complete capture-to-display timing remain unavailable.

The current local CPU development probe has exercised three native windows,
nine frames and three actual GUI captures. Formal data admission,
cross-camera accuracy, two complete qualification runs and the full failure
matrix remain incomplete. Development footage or successful process exits
do not replace those checks.

The canonical MiniNDN topology has exactly five network nodes:

```text
uav1 ─┐
uav2 ─┼─ compute ─ gs
uav3 ─┘
```

`uav1`, `uav2`, and `uav3` are the three drone camera/source nodes; `compute`
is the only processing/tracking/inference node; `gs` is the Ground Station.
The Controller is a separate process and identity inside `gs`, and the
compute-host renderer is a separate process inside `compute`; neither is a
sixth MiniNDN node. The manifest therefore has five network nodes and seven
application processes in `compat` mode. Main mode instead has six top-level
applications: Controller, GS, three camera applications and compute; worker
children are counted separately by the run receipt. Every node has its own NFD; no extra forwarder is
allowed.

`run_multicamera_tracking_demo.py` is the only Spec191 entrypoint. `--prepare`
creates a hash-bound manifest without downloading or copying the large local
assets. `--preflight` validates the offline candidate. `--run` starts bounded
process groups only after native binaries, root MiniNDN privileges, and the
requested display mode pass. Use `--headless` only for pipeline automation; it
does not satisfy the three-window desktop criterion.

Pass the video directory and model path explicitly with `--source` and
`--model`; the runner binds their hashes into the manifest and does not download
or copy them. Results must be collected through validated NDNSF named Data; a
path, process exit, or preflight JSON is not a result. Runtime evidence belongs
under the ignored run directory and the Spec191 evidence files record whether
each gate is implemented, executed, or measured.

Legacy and explicit Spec210 modes use a run-local fixed-root trust policy by
default. Offline preparation writes the public policy under
`private/config/uav-trust-schema.conf`; it creates no private keys. At run time,
the maintained `ndnsec` provisioner creates `/example/uav` and the six required
root-signed actor certificates in the run's private keychain, records public
certificate hashes in `manifest.json`, and then starts the native applications.
Use a fresh output directory for each network attempt. A second owner or an
already populated managed keychain is rejected before credentials are changed.

A distinct explicit `--trust-schema` remains caller-owned: provision its fixed
root and matching actor certificates in the run's keychain before running.
The runner pins that file and preserves those credentials. Arbitrary custom
schema syntax and certificate chains are checked by the native validator;
offline preflight does not certify them. `type any` is rejected by the native
fixed-root policy. Compatibility runtime qualification is recorded separately
from plan construction and Python wiring tests.

## Spec210 metric observation

The same entrypoint supports per-frame, per-camera speed, course, and
camera-relative range from verified v2 observations. The default
`uav_observation_demo.json` profile is simulated and illustrative; its output
is explicitly unvalidated. `--observation-mode replay` preserves source PTS
while changing delivery timing. `--observation-mode live-push` can use a
reference-video adapter that decodes each selected frame on demand and writes
to bounded FIFOs, or an external adapter selected by a profile whose
`source.kind` is `external-fd`.

The default profile assumes hovering drones: all three `velocityWmps` vectors
are `(0,0,0)` with fixed poses. The same profile field and observation API can
later supply slow drift or real poses. The reference videos have no matching
flight telemetry, so all outputs remain simulated, illustrative and unvalidated;
stationary or uncertain tracks retain unknown course under unchanged quality gates.

For an external adapter, pass its executable with `--external-source-adapter`.
The runner starts one adapter process per camera and waits for each process to
write the exact line `UAV_OBSERVATION_SOURCE_ADAPTER_READY` to stdout before it
starts the camera nodes. Print the marker after initialization and before any
FIFO open that could block; then write ordered `uav-source-event-v2` JSONL
records to the supplied per-window FIFOs, honoring pipe backpressure.
When `--source-switch-after-window` is used, the adapter must also load the
`--source-generations` manifest and include
`generation-manifest-sha256=sha256:<64 lowercase hex digits>` in its READY line.
Without a source switch, the base READY line is sufficient.

For a reproducible three-view detector probe, the Apache-2.0
[UAV video dataset](https://huggingface.co/datasets/jye9/Multi-Camera-Multi-Vehicle-Tracking-System)
provides `uav1_1min.mp4`, `uav2_1min.mp4`, and `uav3_1min.mp4`; the matching
[model repository](https://huggingface.co/jye9/Multi-Camera-Multi-Vehicle-Tracking-System)
provides `3UAVs_YOLOv8n.pt`. Download these separately, preserve their
attribution/license, then pass their local paths explicitly:

```sh
python3 Experiments/UAV/run_multicamera_tracking_demo.py \
  --source "$ASSET_DIR" --model "$MODEL_PATH" --license Apache-2.0 \
  --output-dir "$RUN_DIR" --prepare --preflight --headless \
  --window-count 100 --observation-mode replay
```

Those videos do not include synchronized GPS/IMU or camera calibration truth.
Keep `acquisitionKind` simulated and `accuracyQualification` unvalidated unless
the profile is replaced with measurements from a calibrated source.

For real hardware integration, set `acquisitionKind` to `measured` while
keeping `accuracyQualification` unvalidated, and run with
`--observation-mode live-push`. The external adapter supplies calibration,
ground, pose/clock, and exact frame bytes through the native
`StreamObservationSource` API or the Python source-event writer. It does not
need to expose an MP4 path to the compute node. Per-window JSON and matching
CSV exports are kept under `private/compute/worker/`; null metrics remain blank
in CSV and carry explicit status/reason fields in JSON. A simulated profile
must never be relabeled as measured.

When calibration records are published by a separate delegated identity, start
`UavTrackingGroundStation` with
`--calibration-provider /example/uav/calibration-provider`. The Ground Station
then selects that exact identity as the `Calibration-Provider` participant in
each window. The provider publishes its original signed immutable record; each
camera observation carries the exact name, signer identity, and content digest.
Without this option, the existing three-camera plus Compute collaboration is
unchanged.

## Injected motion and vehicle-speed estimate

The Python entrypoint creates a run-private `private/motion/` fixture for each
window. It contains source-time-aligned GPS-like position, heading, altitude,
and UAV speed (`groundSpeedMmps`; `groundspeed_mps` is accepted only as an
input alias), plus one calibration/homography per camera and an independent
vehicle trajectory marked `simulated-input`. The Ground Station includes the
window record and digest in the authenticated collaboration request. Source
providers bind it to the frame envelope; compute writes only the verified
window record beside the verified frame bytes and passes it to the Python
worker.

The worker projects each tracked box's bottom-center through the calibrated
homography, uses source-time `deltaUs`, and subtracts the injected UAV ego
motion. Results report `vehicleSpeedMmps` with `homography-estimated` or
`unknown` provenance. Missing/stale telemetry, invalid or reversed calibration,
PTS disorder, and disabled ego compensation fail closed; the fixture's vehicle
trajectory is an oracle only and is never copied into a track or response as
an estimate. Use `--motion-fixture PATH` to replay an existing validated
fixture directory.
