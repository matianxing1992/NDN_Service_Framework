# Spec 164 MiniNDN Performance Report

Canonical campaign: `/home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-campaign-20260730T050211Z`

Manifest SHA-256: `1ec7305d0b2f6b563ac7a65bf3858a82d3e64fdee57fab91c3202ec36f3636b2`

## Outcome

| Criterion | Verdict | Interpretation |
|---|---|---|
| SC-002 digest/raw >= 0.85 for >=64 MiB | INCONCLUSIVE | Point estimate passes, but n=5 bootstrap lower bound does not; no inferential throughput claim. |
| SC-003 signed/digest >= 0.90 | FAIL | At least one admitted workload has a median paired ratio below 0.90; negative result retained. |
| SC-007 read <=1.20x, write <=1.50x | INCONCLUSIVE | Write=PASS; read=NOT_MEASURED. |
| SC-011 complete formal cells | PASS | 24 warmups and 120 measured samples retained; five measured repetitions per admitted cell. |
| SC-012 MiniNDN before TigerCluster | PASS | Correctness, recovery, compatibility, migration, and campaign evidence exist; no TigerCluster claim is made. |

## Principal paired ratios

| Pair | Comparison | Median | 95% bootstrap CI | Min | Max |
|---|---|---:|---:|---:|---:|
| s1048576-r1-c1 | digest-only / raw | 0.978 | [0.939, 2.059] | 0.939 | 2.059 |
| s1048576-r1-c1 | legacy-exact-packet / raw | 0.437 | [0.374, 1.007] | 0.374 | 1.007 |
| s1048576-r1-c1 | signed-manifest / raw | 0.962 | [0.585, 1.310] | 0.585 | 1.310 |
| s1048576-r1-c16 | digest-only / raw | 0.949 | [0.574, 1.176] | 0.574 | 1.176 |
| s1048576-r1-c16 | legacy-exact-packet / raw | 0.228 | [0.184, 0.347] | 0.184 | 0.347 |
| s1048576-r1-c16 | signed-manifest / raw | 0.761 | [0.698, 1.479] | 0.698 | 1.479 |
| s1048576-r1-c4 | digest-only / raw | 0.923 | [0.752, 1.114] | 0.752 | 1.114 |
| s1048576-r1-c4 | legacy-exact-packet / raw | 0.653 | [0.426, 0.807] | 0.426 | 0.807 |
| s1048576-r1-c4 | signed-manifest / raw | 0.936 | [0.656, 1.323] | 0.656 | 1.323 |
| s1048576-r3-c1 | digest-only / raw | 0.943 | [0.700, 2.234] | 0.700 | 2.234 |
| s1048576-r3-c1 | legacy-exact-packet / raw | 0.443 | [0.415, 1.131] | 0.415 | 1.131 |
| s1048576-r3-c1 | signed-manifest / raw | 0.996 | [0.569, 1.330] | 0.569 | 1.330 |
| s1048576-r3-c4 | digest-only / raw | 0.552 | [0.423, 1.095] | 0.423 | 1.095 |
| s1048576-r3-c4 | legacy-exact-packet / raw | 0.197 | [0.173, 0.410] | 0.173 | 0.410 |
| s1048576-r3-c4 | signed-manifest / raw | 0.456 | [0.364, 0.810] | 0.364 | 0.810 |
| s67108864-r1-c1 | digest-only / raw | 0.992 | [0.364, 1.191] | 0.364 | 1.191 |
| s67108864-r1-c1 | legacy-exact-packet / raw | 0.360 | [0.326, 0.784] | 0.326 | 0.784 |
| s67108864-r1-c1 | signed-manifest / raw | 0.942 | [0.418, 1.092] | 0.418 | 1.092 |
| s1048576-r1-c1 | signed / digest | 0.965 | [0.623, 0.990] | 0.623 | 0.990 |
| s1048576-r1-c16 | signed / digest | 0.893 | [0.647, 1.522] | 0.647 | 1.522 |
| s1048576-r1-c4 | signed / digest | 1.014 | [0.868, 1.348] | 0.868 | 1.348 |
| s1048576-r3-c1 | signed / digest | 0.929 | [0.596, 1.577] | 0.596 | 1.577 |
| s1048576-r3-c4 | signed / digest | 0.825 | [0.716, 1.079] | 0.716 | 1.079 |
| s67108864-r1-c1 | signed / digest | 0.986 | [0.873, 1.149] | 0.873 | 1.149 |

## Failures and admissibility

- Formal measured failures: 0/120.
- Warmup failures: 1/24.
- The retained warmup failure was a single r3/c1 digest-only replica timeout; the other two replicas completed.
- Preflight admitted 24 of 96 candidates. It excluded the other 72 before formal outcomes using frozen disk, memory, 60-second predicted-run, and legacy metadata-row gates.

## Scaling and physical ceiling

- Physical path bottleneck: 659.613 Mbit/s (four directional 60-second measurements).
- Concurrency comparisons retained: 12; replica comparisons: 8; size comparisons: 4.
- Full scaling ratios, efficiency, per-cell amplification distributions, and physical-ceiling utilization are in `derived-results.json`.

## Measurement interpretation

- `logicalGoodputMbps` counts application bytes once per logical operation. For replicated publication, physical wire and storage work include all replicas.
- `wireGoodputMbps` uses received Data wire bytes; Interest wire bytes are counted separately by Interest count, not included in this byte total.
- Write amplification divides physical retained payload/database bytes by logical payload bytes times committed replica count.
- Phase durations may overlap and are not summed into end-to-end latency.
- Confidence intervals are deterministic 10,000-resample percentile bootstrap intervals for the median.

## Limitations

- Five measured repetitions close only the engineering gate; they do not support an inferential paper claim.
- Only the 64-MiB r1/c1 cell was admissible at or above 64 MiB; 1-GiB and 16-GiB cells were mechanically excluded before formal outcomes.
- The campaign measures publication Data transfer, packet trust, and persistence subjects; its benchmark consumer is not the public NDNSF Collaboration control path.
- Discovery, reservation, replication-control, and activation-control timings are placeholders in this data-plane microbenchmark; SC-004 is not established.
- No cold retrieval was measured, so payload-store read amplification and the complete SC-007 gate remain inconclusive.
- The single warmup timeout is retained; no measured run failed.
- This is MiniNDN evidence, not TigerCluster or large-model evidence.

## Reproduction

```bash
python3 Experiments/analyze_distributed_repo_artifact.py \
  --campaign /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-campaign-20260730T050211Z \
  --output-json /home/tianxing/NDN/ndn-service-framework/results/spec164-artifact-campaign-20260730T050211Z/derived-results.json \
  --output-markdown specs/164-distributed-repo-large-artifact-transport/evidence/performance-report.md
```

The analyzer verifies the manifest seal and independently recomputes run
counts, uniqueness, measured repetitions, and paired ratios from the CSV
ledger using `Decimal` arithmetic.
