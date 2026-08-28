# Spec 126 Completion Summary

## Verdict

**PASS.** The complete, newly named confirmation is
`results/spec126-loss-reorder-20260720-confirmation07`. It contains exactly 16
unique cells and 16 CSV rows. Every command ran once; `automaticRetry=false`,
`sourceUnchanged=true`, and `spec125EvidenceUnchanged=true`.

Exact command:

```bash
python3 Experiments/run_spec126_loss_reorder_matrix.py \
  --output-root results/spec126-loss-reorder-20260720-confirmation07 \
  --duration-seconds 60
```

## Frozen acceptance

| Treatment | Accepted | Required | Exact 95% interval | Verdict |
|---|---:|---:|---:|---|
| zero-loss | 1/1 | 1 | [0.0250, 1.0000] | PASS |
| isolated loss | 4/5 | 4 | [0.2836, 0.9949] | PASS |
| bounded reorder | 5/5 | 5 | [0.4782, 1.0000] | PASS |
| combined | 4/5 | 4 | [0.2836, 0.9949] | PASS |

The two non-accepted results remain evidence. `isolated-loss-run-04` ended with
a process/lifecycle failure despite otherwise passing measured stream checks.
`combined-run-01` produced only 30 decoded frames and failed final-window
continuity, future-hit, and p95 checks. Neither cell was rerun.

## Traffic attribution

Values below are arithmetic means across every planned repetition, including
failed repetitions. Mapping new-information ratio is
`mappingNewDataResponses / mappingDataResponses`.

| Treatment | Payload Interests | Mapping Interests | Mapping Data | New Mapping Data | New-info ratio | retries | timeouts | Nacks | future-hit |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zero-loss | 3699.0 | 1845.0 | 1842.0 | 1842.0 | 1.0000 | 0.0 | 17.0 | 0.0 | 0.9943 |
| isolated loss | 3693.0 | 1880.0 | 1840.2 | 1840.2 | 1.0000 | 39.4 | 164.0 | 0.0 | 0.9951 |
| bounded reorder | 3700.2 | 1850.4 | 1842.0 | 1842.0 | 1.0000 | 5.4 | 22.4 | 0.0 | 0.9954 |
| combined | 2977.0 | 1512.0 | 1482.0 | 1482.0 | 1.0000 | 28.4 | 134.0 | 0.0 | 0.9276 |

The combined mean is intentionally depressed by the retained failed first
repetition. Every accepted impaired run exceeds 99.4% Provider-confirmed
future hits, remains far below the 25% payload-Interest overhead bound, and
stays below the 300 ms p95 / 600 ms p99 exact capture-to-decode limits.

## Implementation closure

- Out-of-order Mapping is admitted by authenticated continuity rather than
  arrival order.
- Mapped-live scheduling restores a final wakeup without erasing congestion
  memory, and caps decision horizon to expressible aggregate capacity.
- Mapping and payload work have separate bounded budgets and observable retry,
  timeout, and Nack outcomes.
- Automated test shutdown uses a bounded idempotent retry; interactive runtime
  behavior is unchanged.
- No UAV/codec-specific policy was added to Core.
