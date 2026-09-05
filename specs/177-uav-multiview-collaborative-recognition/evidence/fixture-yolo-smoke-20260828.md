# Generated Fixture Single-View YOLO Smoke

**Date**: 2026-08-28

**Scope**: Functional image suitability only. This is not multi-view fusion, NDNSF delivery, or recognition-accuracy evidence.

## Command profile

Each image in `NDNSF-UAV-APP/testdata/multiview-car/` was passed independently to:

```bash
MPLCONFIGDIR=/tmp/ndnsf-mpl \
python3 NDNSF-UAV-APP/tools/yolo_detect_once.py \
  --image <view.png> --conf 0.01
```

Model: `yolo26n.pt`

Model SHA-256: `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`

The downloaded artifact was moved out of the repository to the local content-addressed cache after the run.

## Results

| View | Detected class | Car confidence | Count |
| --- | --- | ---: | ---: |
| front-left, 12 m | Car | 0.954 | 1 |
| front-right, 22 m | Car | 0.937 | 1 |
| rear-left, 10 m | Car | 0.950 | 1 |
| rear-right, 30 m | Car | 0.806 | 2 |
| high-left, 18 m | Car | 0.949 | 1 |
| right profile, 15 m | Car | 0.941 | 1 |

The 30 m image produced a duplicate car box and therefore also provides a useful non-max-suppression/target-correlation edge case.

## Interpretation

- All six PNG files decode and produce at least one real model car detection.
- The fixture is suitable for implementing image decoding, crop extraction, feature pooling, annotation, and app transport tests.
- The low confidence threshold was chosen for fixture qualification and is not a deployment threshold.
- No multi-view model was executed, so this evidence does not satisfy Spec 177 T005, G3, or any accuracy claim.
