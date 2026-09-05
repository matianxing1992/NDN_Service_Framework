# Spec 177 controlled evaluation evidence

**Run date**: 2026-08-29  
**Source revision**: `UAV-Experimental` working tree (implementation changes
are intentionally not submitted with the Spec177 design/images)  
**Dataset**: `generated-blue-suv-multiview-controlled-v1`  
**Registration SHA-256**: `40733e487bbba186b492e1ad91d374f8e7524dafae4540b9b4ac0ff3c47371e3`  
**Evaluation SHA-256**: `1bc597d8d3849cc073d3cc4582cb75291cc9a6ef920a182d054fdc2bba55f977`

The registration uses the six checked-in generated car views as one synchronized
functional sample.  It records nominal camera metadata and a class label, but
sets `scientificAccuracyClaimAllowed=false`: the images are not calibrated
flight data and this run does not support a recognition-accuracy claim.

## Reproduction

```bash
python3 NDNSF-UAV-APP/tools/prepare_coperception_uav.py \
  --source NDNSF-UAV-APP/testdata/multiview-car \
  --output results/uav-multiview-controlled-20260828/registration.json \
  --dataset-id generated-blue-suv-multiview-controlled-v1 \
  --license internal-functional-fixture
python3 NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py \
  --registration results/uav-multiview-controlled-20260828/registration.json \
  --output results/uav-multiview-controlled-20260828/evaluation.json
```

## Observed functional metrics

| Prefix views | Completion | Accepted views | Point accuracy* | Mean local worker latency | Mean bytes read |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.000 | 1 | 1.000 | 1417.841 ms | 2,584,428 |
| 2 | 1.000 | 2 | 1.000 | 2454.647 ms | 5,153,577 |
| 4 | 1.000 | 4 | 1.000 | 4211.925 ms | 10,233,397 |
| 6 | 1.000 | 6 | 1.000 | 5927.085 ms | 15,623,019 |

\*The label is the synthetic fixture label and the worker uses a deterministic
CPU fallback when no model artifact is supplied.  The point value is retained
only as a contract sanity check, not as scientific model accuracy.  Latency and
bytes are local image decoding/materialization measurements, not NDNSF network
latency or provider-throughput measurements.

The run verifies paired prefix grouping, exact view-count provenance, one
annotation per accepted view, and a different pooled-feature digest for each
view count.  Scientific evaluation remains pending a registered calibrated
multi-UAV dataset with multiple samples and paired uncertainty analysis.

The evaluator now records per-row input-manifest and result hashes, failure
stage counts, model-registry provenance, and paired-bootstrap metadata.  This
registration contains one sample, so the recorded uncertainty status is
`insufficient-samples` (2000 bootstrap replicates are not interpreted as a
confidence interval).  The campaign is therefore a reproducible functional
point-estimate gate, not evidence of a multi-view accuracy or latency benefit.
