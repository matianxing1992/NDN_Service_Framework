# Spec 167 TigerCluster performance report

Campaign: `spec167-tiger-20260802-source013`  
Evidence: `/project/tma1/ndnsf-di/evidence/spec167/campaign/spec167-tiger-20260802-source013`  
Local mirror: `results/spec167-source-013-freeze/remote-evidence/`

## Acceptance result

The source-013 formal campaign used two CPU-only nodes (`itiger07,itiger08`)
and the frozen 60-row schedule. Job `181822` completed in `02:34:53` with exit
code 0. The analyzer returned `SPEC167_ANALYSIS_PASS`:

- 60/60 run records observed; 10 warmups and 50 measured repetitions;
- all 60 records have status `PASS`;
- zero measured failures, missing rows, duplicates, unexpected rows, or path
  violations;
- remote and local checksum ledgers both pass.

The source-012 `ModuleNotFoundError` is not included in these measurements. It
was a packaging-closure failure before repository transport and is retained as
immutable negative evidence in `evidence/source-012-freeze.md`.

## Median logical goodput

Values are Mbps over the five measured repetitions in each subject/size cell.

| Payload | Physical network | Raw segmented NDN | Digest-only | Signed manifest | Legacy exact packet |
|---:|---:|---:|---:|---:|---:|
| 64 MiB | 940.852 | 332.191 | 325.870 | 323.942 | 52.141 |
| 1 GiB | 940.853 | 336.125 | 327.271 | 328.983 | 52.334 |

The matched signed/digest median ratios are 0.994 (64 MiB) and 1.007 (1 GiB);
signed/legacy medians are 6.198x and 6.263x. Digest/raw medians are 0.981 and
0.972. These ratios are descriptive matched observations, not a claim of
line-rate saturation.

## Cold and warm state separation

The run records retain separate cold-transfer fields and warm-reuse counters.
Across all 60 records, 48 cold records report non-zero cold logical bytes and
every cold logical-byte count equals its corresponding logical-byte count. No
record reports a digest/integrity failure, timeout, or retransmission. The
aggregate `duplicatePayloadBytesWritten` is zero; warm reuse therefore did not
write duplicate payload bytes. Cold and warm measurements remain separate in
the retained record ledger and were not pooled in the analyzer's goodput
ratios.

## Metric boundary and follow-up

This campaign is a transport/goodput validation. The retained result schema
does not emit host CPU utilization, peak memory, Data/Interest wire-byte
breakdowns, or a complete read/write amplification decomposition. Those values
must not be inferred from logical goodput and are not claimed here. A follow-up
instrumentation task should add these counters before making a resource-cost or
wire-overhead claim. The per-run elapsed time, logical bytes, subtransfer count,
timeouts, retransmissions, cold bytes, warm-reuse count, and duplicate-byte
counter are retained and independently checkable.

