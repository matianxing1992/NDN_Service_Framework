# Spec 164 MiniNDN Performance Report

Canonical campaign: `/home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-stability-campaign-20260730T0935Z`

Manifest SHA-256: `282b63e0003889c3785e1ec6a70b7b8b8784d3cc32c54ea58dd3787b3b7e7532`

## Outcome

| Criterion | Verdict | Interpretation |
|---|---|---|
| SC-002 digest/raw >= 0.85 for >=64 MiB | PASS | Completion=PASS; point and bootstrap gates are reported separately. |
| SC-003 signed/digest >= 0.90 | FAIL | Completion=FAIL; failed and zero-goodput samples remain negative outcomes. |
| SC-004 bounded control operations | PASS | 2 operations for 16 Data segments; no per-segment service invocation. |
| SC-007 read <=1.20x, write <=1.50x | PASS | Write=PASS; read=PASS. |
| SC-011 complete formal cells | PASS | 24 warmups and 120 measured samples retained; five measured repetitions per admitted cell. |
| SC-012 MiniNDN before TigerCluster | PASS | Correctness, recovery, compatibility, migration, and campaign evidence exist; no TigerCluster claim is made. |

## Principal paired ratios

| Pair | Comparison | Median | 95% bootstrap CI | Min | Max |
|---|---|---:|---:|---:|---:|
| s1048576-r1-c1 | digest-only / raw | 1.069 | [1.028, 1.159] | 1.028 | 1.159 |
| s1048576-r1-c1 | legacy-exact-packet / raw | 0.726 | [0.688, 0.788] | 0.688 | 0.788 |
| s1048576-r1-c1 | signed-manifest / raw | 1.028 | [0.935, 1.148] | 0.935 | 1.148 |
| s1048576-r1-c16 | digest-only / raw | 0.930 | [0.000, 1.210] | 0.000 | 1.210 |
| s1048576-r1-c16 | legacy-exact-packet / raw | 0.235 | [0.000, 0.265] | 0.000 | 0.265 |
| s1048576-r1-c16 | signed-manifest / raw | 0.940 | [0.690, 1.048] | 0.690 | 1.048 |
| s1048576-r1-c4 | digest-only / raw | 0.967 | [0.845, 1.336] | 0.845 | 1.336 |
| s1048576-r1-c4 | legacy-exact-packet / raw | 0.527 | [0.433, 0.677] | 0.433 | 0.677 |
| s1048576-r1-c4 | signed-manifest / raw | 1.060 | [0.883, 1.489] | 0.883 | 1.489 |
| s1048576-r3-c1 | digest-only / raw | 1.298 | [1.021, 1.442] | 1.021 | 1.442 |
| s1048576-r3-c1 | legacy-exact-packet / raw | 0.781 | [0.639, 0.855] | 0.639 | 0.855 |
| s1048576-r3-c1 | signed-manifest / raw | 1.046 | [0.981, 1.283] | 0.981 | 1.283 |
| s1048576-r3-c4 | digest-only / raw | 0.985 | [0.804, 1.109] | 0.804 | 1.109 |
| s1048576-r3-c4 | legacy-exact-packet / raw | 0.301 | [0.268, 0.345] | 0.268 | 0.345 |
| s1048576-r3-c4 | signed-manifest / raw | 0.921 | [0.810, 1.010] | 0.810 | 1.010 |
| s67108864-r1-c1 | digest-only / raw | 1.015 | [0.937, 1.171] | 0.937 | 1.171 |
| s67108864-r1-c1 | legacy-exact-packet / raw | 0.692 | [0.643, 0.822] | 0.643 | 0.822 |
| s67108864-r1-c1 | signed-manifest / raw | 1.011 | [1.001, 1.176] | 1.001 | 1.176 |
| s1048576-r1-c1 | signed / digest | 0.933 | [0.875, 1.001] | 0.875 | 1.001 |
| s1048576-r1-c16 | signed / digest | 0.888 | [0.570, 1.076] | 0.570 | 1.076 |
| s1048576-r1-c4 | signed / digest | 1.114 | [0.815, 1.302] | 0.815 | 1.302 |
| s1048576-r3-c1 | signed / digest | 0.911 | [0.714, 1.201] | 0.714 | 1.201 |
| s1048576-r3-c4 | signed / digest | 0.884 | [0.807, 1.145] | 0.807 | 1.145 |
| s67108864-r1-c1 | signed / digest | 0.996 | [0.989, 1.076] | 0.989 | 1.076 |

## Failures and admissibility

- Formal measured failures: 3/120.
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
  --campaign /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-stability-campaign-20260730T0935Z \
  --output-json /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-stability-campaign-20260730T0935Z/derived-results.json \
  --output-markdown specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t024-performance-report.md \
  --control-evidence /home/tianxing/NDN/ndn-service-framework/results/spec164-public-task-minindn-20260730T0734Z/summary.json
```

The analyzer verifies the manifest seal and independently recomputes run
counts, uniqueness, measured repetitions, and paired ratios from the CSV
ledger using `Decimal` arithmetic.
