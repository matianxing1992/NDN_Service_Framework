# Spec191 UAV tracking experiment

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
application processes. Every node has its own NFD; no extra forwarder is
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
