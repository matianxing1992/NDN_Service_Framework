# T024 Frozen MiniNDN Remediation Campaign

## Outcome

T024 is complete as an evidence-production task. The replacement campaign is
frozen, complete, independently verified, and registered by
`evidence/campaign-pointer.md`. It does **not** close the Phase 9 acceptance
gate because SC-003 is a retained `FAIL`.

```text
campaign: results/spec164-artifact-remediation-campaign-20260730T0820Z
campaignId: spec164-artifact-20260730T072333Z
admitted cells: 24/96
schedule: 144 unique runs
warmup: 24 retained, 16 PASS, 8 FAIL
measured: 120 retained, 77 PASS, 43 FAIL
measured repetitions per cell: 5
independent verification: PASS
```

## Determinate success-criterion verdicts

```text
SC-002: PASS
  completion gate: PASS
  64-MiB digest/raw median: 1.022235
  deterministic median bootstrap 95% CI: [0.985472, 1.053036]

SC-003: FAIL
  completion gate: FAIL
  point-estimate gate over successful pairs: PASS
  bootstrap lower-bound gate: FAIL
  reason: failed/zero-goodput high-concurrency digest and signed runs remain
          negative outcomes and are not removed from the verdict

SC-004: PASS
  public Collaboration control operations: 2
  publication Data segments: 16
  selected replicas: 1
  lifecycle phases: 3
  per-chunk NDNSF service invocation: false

SC-007: PASS
  stable r1/c1 completion and cold-destination gate: PASS
  payload/metadata read-amplification gate: PASS
  payload/metadata write-amplification gate: PASS
```

The 1 MiB r1/c1, 1 MiB r3/c1, and 64 MiB r1/c1 cells were stable. The
1 MiB high-concurrency cells exhibited retained probabilistic transfer
failures. This negative result is not tuned away and prevents a claim that the
current implementation satisfies every Phase 9 performance threshold.

## Immutable evidence

```text
campaign-manifest.json
  45cdfc4602fbdbd4cb4b3fcec1df8f53755f2b8fde1da4f573dcd4cb8023e74e
campaign-runs.jsonl
  d08db5dda0efd82545bd708b2c0f9ad95e25fd683593fefdc60e0e0f220e0c44
campaign-runs.csv
  7104bdd5e067bd27624c7f16156bb8e1f43870d918c49aa2aafc82b4f379576c
derived-results.json
  b76657c7a435156b1bfc5cf265c753fb98dd4b031da91b9342d239501d142481
t024-performance-report.md
  129bbfbbbc2200fc0be34f0156e3a25266a835762f9f47ec32637eb4d6847a91
public Collaboration summary.json
  22e48024507c9a68dc423365f1357edefa95d21132ec679971da54abcb57ae01
```

The predecessor campaign
`results/spec164-artifact-campaign-20260730T050211Z` remains unchanged.

## Verification

```bash
python3 tests/python/test_spec164_performance_analysis.py
# 6 tests PASS

python3 tests/python/test_spec164_performance_harness.py
# 13 tests PASS

sudo -n env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  python3 Experiments/run_spec164_artifact_campaign.py \
  --resume-campaign \
  "$PWD/results/spec164-artifact-remediation-campaign-20260730T0820Z" \
  --skip-physical-ceiling
# SPEC164_ARTIFACT_CAMPAIGN_OK
```

The analyzer explicitly guards zero denominators. A failed digest denominator
does not create a ratio, but its run remains in the completion gate and forces
SC-003 to fail. Scaling ratios with zero baselines are represented as
undefined rather than raising or inventing infinity.
