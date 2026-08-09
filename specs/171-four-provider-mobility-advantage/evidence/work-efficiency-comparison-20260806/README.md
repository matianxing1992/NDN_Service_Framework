# Capacity-matched work-efficiency figure

This publication asset is generated from the frozen six-seed aggregate:

`results/four_provider_work_efficiency_confirmatory_20260806/combined-six-seed-aggregate.json`

The campaign uses seeds 20--25, 1,800 logical requests per system, four
Providers, `block_network=true`, and matched 5 RPS / 60 s measurement windows.
The aggregate verdict is
`NDNSF_MULTIPROVIDER_WORK_EFFICIENCY_ADVANTAGE_CONFIRMED`.

Pooled values rendered in the figure:

| System | Success | Provider/server executions per request |
| --- | ---: | ---: |
| NDNSF | 1,798 / 1,800 (99.89%) | 1.000 |
| gRPC-PAR-4 | 1,800 / 1,800 (100.00%) | 3.989 |
| NSC-4 | 1,800 / 1,800 (100.00%) | 1.002 |

The paired NDNSF minus gRPC success interval is `[-0.22, 0.00]` percentage
points. The NDNSF/gRPC execution-work ratio is `0.250696`. This asset supports
an execution-work-efficiency claim only; it does not make a latency claim
because the NDNSF summary does not expose a directly comparable p95.

Generated outputs:

- `provider-work-efficiency.png` for slides and visual review;
- `provider-work-efficiency.pdf` for the paper;
- `provider-work-efficiency.svg` for editable/vector reuse.
