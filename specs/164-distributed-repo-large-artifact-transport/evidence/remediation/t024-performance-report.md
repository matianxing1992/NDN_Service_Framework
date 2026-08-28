# Spec 164 MiniNDN Performance Report

Canonical campaign: `/home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-remediation-campaign-20260730T0820Z`

Manifest SHA-256: `45cdfc4602fbdbd4cb4b3fcec1df8f53755f2b8fde1da4f573dcd4cb8023e74e`

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
| s1048576-r1-c1 | digest-only / raw | 0.963 | [0.864, 1.201] | 0.864 | 1.201 |
| s1048576-r1-c1 | legacy-exact-packet / raw | 0.749 | [0.602, 0.770] | 0.602 | 0.770 |
| s1048576-r1-c1 | signed-manifest / raw | 0.933 | [0.794, 0.946] | 0.794 | 0.946 |
| s1048576-r1-c16 | digest-only / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r1-c16 | legacy-exact-packet / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r1-c16 | signed-manifest / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r1-c4 | digest-only / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r1-c4 | legacy-exact-packet / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r1-c4 | signed-manifest / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r3-c1 | digest-only / raw | 1.411 | [1.125, 1.502] | 1.125 | 1.502 |
| s1048576-r3-c1 | legacy-exact-packet / raw | 0.851 | [0.654, 0.990] | 0.654 | 0.990 |
| s1048576-r3-c1 | signed-manifest / raw | 1.468 | [1.068, 1.544] | 1.068 | 1.544 |
| s1048576-r3-c4 | digest-only / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r3-c4 | legacy-exact-packet / raw | 0.000 | [0.000, 0.000] | 0.000 | 0.000 |
| s1048576-r3-c4 | signed-manifest / raw | 0.677 | [0.000, 1.353] | 0.000 | 1.353 |
| s67108864-r1-c1 | digest-only / raw | 1.022 | [0.985, 1.053] | 0.985 | 1.053 |
| s67108864-r1-c1 | legacy-exact-packet / raw | 0.679 | [0.672, 0.706] | 0.672 | 0.706 |
| s67108864-r1-c1 | signed-manifest / raw | 1.010 | [1.001, 1.093] | 1.001 | 1.093 |
| s1048576-r1-c1 | signed / digest | 0.919 | [0.776, 1.004] | 0.776 | 1.004 |
| s1048576-r3-c1 | signed / digest | 1.058 | [0.711, 1.153] | 0.711 | 1.153 |
| s67108864-r1-c1 | signed / digest | 1.016 | [0.959, 1.069] | 0.959 | 1.069 |

## Failures and admissibility

- Formal measured failures: 43/120.
- Warmup failures: 8/24.
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
  --campaign /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-remediation-campaign-20260730T0820Z \
  --output-json /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-remediation-campaign-20260730T0820Z/derived-results.json \
  --output-markdown specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t024-performance-report.md \
  --control-evidence /home/tianxing/NDN/ndn-service-framework/results/spec164-public-task-minindn-20260730T0734Z/summary.json
```

The analyzer verifies the manifest seal and independently recomputes run
counts, uniqueness, measured repetitions, and paired ratios from the CSV
ledger using `Decimal` arithmetic.
