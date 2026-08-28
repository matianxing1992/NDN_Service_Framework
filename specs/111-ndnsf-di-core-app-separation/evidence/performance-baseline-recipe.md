# Frozen MiniNDN Performance Baseline Recipe

Date frozen: 2026-07-14  
Harness: `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`  
Scope: local MiniNDN only; no container runtime and no iTiger/Slurm activity.

## Invariants

- Topology: `Experiments/Topology/AI_Lab.conf`; one controller, one requester,
  and three stage Providers.
- Workload: fake deterministic 3-stage/24-layer DI pipeline, one generated
  token, 1 request/s, ten warmup requests outside measurement, 60 offered
  measured requests during exactly 60 measured seconds.
- Runtime knobs: `--compute-delay-ms 1`, ACK timeout 1500 ms, request timeout
  60000 ms, external campaign timeout 240 seconds.
- Logging: `ndn_service_framework.*=WARN`.
- Timeline: `NDNSF_TIMELINE_TRACE=1` and
  `NDNSF_TIMELINE_TRACE_SAMPLE_RATE=100`; the same stable request sampler is
  applied to lifecycle/provider/timeline artifacts.
- Cell identity is carried by `--campaign-id` and the unique output root;
  candidate identity is recorded in the campaign manifest and each terminal
  result beside the source revision/diff and command digest.
- No automatic retry, no replacement cell, no optional stopping. A started
  failed/timeout/malformed cell remains its pair's observed outcome.
- Before every cell, the launcher must prove one writer, an unused output root,
  non-interactive sudo, sufficient disk, and no live MiniNDN/Mininet launcher.

## Diagnostic convergence versus formal evidence

Bug convergence does not restart the matrix after every repair. When a cell
fails, the active sweep stops, the defect is fixed and verified, and the sweep
continues at the next unstarted cell with
`--continue-diagnostic-after-terminal-failure`. Existing PASS, FAIL and
INTERRUPTED terminal records are immutable and are skipped rather than rerun.
The runner records `diagnostic-continuation.json`, candidate identities per
cell, and `formalComparisonEligible=false`; therefore this sweep can find later
defects efficiently but cannot satisfy SC-007.

After all 20 positions have been traversed and known defects are fixed, run one
new clean campaign root from cell 1 through cell 20 using one final treatment
candidate and without the diagnostic continuation flag. Only that clean,
single-candidate, all-PASS campaign is eligible for the paired non-regression
analysis. A failure in the clean campaign remains observed evidence and returns
the workflow to diagnostic convergence; it does not trigger an automatic rerun.

## Exact command template

```bash
sudo -n -E env \
  PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/Experiments" \
  PYTHONHASHSEED="$CELL_SEED" \
  NDNSF_TIMELINE_TRACE=1 \
  NDNSF_TIMELINE_TRACE_SAMPLE_RATE=100 \
  timeout 240s python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
    --topology-file Experiments/Topology/AI_Lab.conf \
    --output-dir "$CELL_OUTPUT" \
    --campaign-id "$CELL_ID" \
    --runtime fake --stages 3 --layers 24 \
    --compute-delay-ms 1 \
    --warmup-requests 10 \
    --measured-requests 60 \
    --measured-duration-s 60 \
    --request-interval-ms 1000 \
    --max-new-tokens 1 \
    --ack-timeout-ms 1500 \
    --timeout-ms 60000 \
    --ndn-log 'ndn_service_framework.*=WARN'
```

The pre-separation canary fixes:

```text
CELL_ID=spec111-pre-separation-canary
CELL_SEED=11100
CELL_OUTPUT=results/spec111-core-app-separation/pre-separation-canary
```

## Ten-pair order and seeds

The final campaign contains ten baseline/treatment pairs (20 cells), matching
SC-007. Odd pairs run baseline then treatment; even pairs reverse the order.
This ordering is frozen before implementation and must not be changed after any
result is visible.

| Pair | Seed | Cell order |
|---:|---:|---|
| 1 | 11101 | baseline, treatment |
| 2 | 11102 | treatment, baseline |
| 3 | 11103 | baseline, treatment |
| 4 | 11104 | treatment, baseline |
| 5 | 11105 | baseline, treatment |
| 6 | 11106 | treatment, baseline |
| 7 | 11107 | baseline, treatment |
| 8 | 11108 | treatment, baseline |
| 9 | 11109 | baseline, treatment |
| 10 | 11110 | treatment, baseline |

The baseline is the immutable pre-movement source snapshot. The treatment is
the frozen post-separation source/candidate. Both must consume identical inputs
and parameters; their output roots are distinct and immutable.

## Analysis contract

For each pair compute treatment relative to baseline for completion, failures,
p50, p95, throughput, RSS/resource and queue evidence. Report all cells, the
median paired relative latency and throughput changes, and a deterministic 95%
paired bootstrap interval using analysis seed `11195`. Any correctness or
completion regression blocks deletion. If either latency or throughput
non-regression interval crosses the declared 5% degradation margin, compatibility
deletion is blocked and rollback is recorded. Failed cells are not imputed or
replaced.
