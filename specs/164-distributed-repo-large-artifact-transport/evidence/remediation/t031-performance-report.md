# Spec 164 MiniNDN Performance Report

Canonical campaign: `/home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-confirmatory-campaign-20260730T1030Z`

Manifest SHA-256: `0fbcd3d4f77dbadba5e90eaad3a23d8425f9a67e0fbd9fca401cc38aff568996`

## Outcome

| Criterion | Verdict | Interpretation |
|---|---|---|
| SC-002 digest/raw >= 0.85 for >=64 MiB | PASS | Completion=PASS; point and bootstrap gates are reported separately. |
| SC-003 signed/digest >= 0.90 | PASS | Completion=PASS; failed and zero-goodput samples remain negative outcomes. |
| SC-004 bounded control operations | PASS | 2 operations for 16 Data segments; no per-segment service invocation. |
| SC-007 read <=1.20x, write <=1.50x | PASS | Write=PASS; read=PASS. |
| SC-011 complete formal cells | PASS | 24 warmups and 120 measured samples retained; five measured repetitions per admitted cell. |
| SC-012 MiniNDN before TigerCluster | PASS | Correctness, recovery, compatibility, migration, and campaign evidence exist; no TigerCluster claim is made. |

## Principal paired ratios

| Pair | Comparison | Median | 95% bootstrap CI | Min | Max |
|---|---|---:|---:|---:|---:|
| s1048576-r1-c1 | digest-only / raw | 0.957 | [0.412, 1.019] | 0.412 | 1.019 |
| s1048576-r1-c1 | legacy-exact-packet / raw | 0.465 | [0.442, 0.485] | 0.442 | 0.485 |
| s1048576-r1-c1 | signed-manifest / raw | 0.950 | [0.909, 1.003] | 0.909 | 1.003 |
| s1048576-r1-c16 | digest-only / raw | 1.062 | [0.838, 1.262] | 0.838 | 1.262 |
| s1048576-r1-c16 | legacy-exact-packet / raw | 0.280 | [0.245, 0.301] | 0.245 | 0.301 |
| s1048576-r1-c16 | signed-manifest / raw | 1.001 | [0.954, 1.106] | 0.954 | 1.106 |
| s1048576-r1-c4 | digest-only / raw | 1.065 | [0.887, 1.180] | 0.887 | 1.180 |
| s1048576-r1-c4 | legacy-exact-packet / raw | 0.465 | [0.440, 0.507] | 0.440 | 0.507 |
| s1048576-r1-c4 | signed-manifest / raw | 0.929 | [0.836, 1.126] | 0.836 | 1.126 |
| s1048576-r3-c1 | digest-only / raw | 0.928 | [0.764, 1.162] | 0.764 | 1.162 |
| s1048576-r3-c1 | legacy-exact-packet / raw | 0.423 | [0.234, 0.775] | 0.234 | 0.775 |
| s1048576-r3-c1 | signed-manifest / raw | 0.801 | [0.616, 1.335] | 0.616 | 1.335 |
| s1048576-r3-c4 | digest-only / raw | 0.646 | [0.566, 0.762] | 0.566 | 0.762 |
| s1048576-r3-c4 | legacy-exact-packet / raw | 0.224 | [0.211, 0.300] | 0.211 | 0.300 |
| s1048576-r3-c4 | signed-manifest / raw | 0.591 | [0.517, 0.763] | 0.517 | 0.763 |
| s67108864-r1-c1 | digest-only / raw | 0.975 | [0.925, 1.015] | 0.925 | 1.015 |
| s67108864-r1-c1 | legacy-exact-packet / raw | 0.363 | [0.334, 0.695] | 0.334 | 0.695 |
| s67108864-r1-c1 | signed-manifest / raw | 0.972 | [0.957, 1.005] | 0.957 | 1.005 |
| s1048576-r1-c1 | signed / digest | 0.998 | [0.950, 2.209] | 0.950 | 2.209 |
| s1048576-r1-c16 | signed / digest | 0.922 | [0.793, 1.293] | 0.793 | 1.293 |
| s1048576-r1-c4 | signed / digest | 0.942 | [0.787, 1.027] | 0.787 | 1.027 |
| s1048576-r3-c1 | signed / digest | 0.855 | [0.664, 1.149] | 0.664 | 1.149 |
| s1048576-r3-c4 | signed / digest | 0.979 | [0.879, 1.045] | 0.879 | 1.045 |
| s67108864-r1-c1 | signed / digest | 0.997 | [0.984, 1.069] | 0.984 | 1.069 |

## Failures and admissibility

- Formal measured failures: 0/120.
- Warmup failures: 0/24.
- Failures are retained by run ID and participate in completion gates; no failed sample is silently removed from a ratio.
- Preflight admitted 24 of 96 candidates. It excluded the other 72 before formal outcomes using frozen disk, memory, 60-second predicted-run, and legacy metadata-row gates.

## Scaling and physical ceiling

- Physical path bottleneck: 0.000 Mbit/s (verdict=NOT_MEASURED).
- Concurrency comparisons retained: 12; replica comparisons: 8; size comparisons: 4.
- Full scaling ratios, efficiency, per-cell amplification distributions, and physical-ceiling utilization are in `derived-results.json`.

## Measurement interpretation

- `logicalGoodputMbps` counts application bytes once per logical operation. For replicated publication, physical wire and storage work include all replicas.
- `wireGoodputMbps` uses total observed Data plus Interest wire bytes; both components and their equality to `wireBytes` are retained separately.
- Write amplification divides physical retained payload/database bytes by logical payload bytes times committed replica count.
- Phase durations may overlap and are not summed into end-to-end latency.
- Confidence intervals are deterministic 10,000-resample percentile bootstrap intervals for the median.

## Limitations

- Five measured repetitions close only the engineering gate; they do not support an inferential paper claim.
- Only the 64-MiB r1/c1 cell was admissible at or above 64 MiB; 1-GiB and 16-GiB cells were mechanically excluded before formal outcomes.
- The data-plane campaign is paired with a separate public NDNSF Collaboration smoke for SC-004; their latency samples are not combined.
- Discovery, ACK collection, planning, queue wait, and activation control are not included in data-plane goodput.
- Cold retrieval uses a fresh destination after publication and records payload-store plus metadata-store reads separately.
- All warmup and measured failures are retained; failed transfers contribute to completion gates and are never removed from threshold verdicts.
- This is MiniNDN evidence, not TigerCluster or large-model evidence.

## Reproduction

```bash
python3 Experiments/analyze_distributed_repo_artifact.py \
  --campaign /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-confirmatory-campaign-20260730T1030Z \
  --output-json /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-confirmatory-campaign-20260730T1030Z/derived-results.json \
  --output-markdown specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t024-performance-report.md \
  --control-evidence /home/tianxing/NDN/ndn-service-framework/results/spec164-public-task-minindn-20260730T0734Z/summary.json
```

The analyzer verifies the manifest seal and independently recomputes run
counts, uniqueness, measured repetitions, and paired ratios from the CSV
ledger using `Decimal` arithmetic.
