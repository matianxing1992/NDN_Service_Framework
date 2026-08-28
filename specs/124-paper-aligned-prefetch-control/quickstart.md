# Quickstart: Paper-Aligned Prefetch Control

## Focused gate

```bash
./build/unit-tests --run_test=Stream
```

## Relevant UAV gate

```bash
./build/unit-tests --run_test=UavProtocolState
```

## Network gate

Use the existing Spec 123 original-load MiniNDN command with a unique `spec124-paper-control-<timestamp>` result identity. Preserve the first admissible 60-second outcome and analyze it with `Experiments/analyze_stream_latency.py`.

Expected evidence: activity in every five-second bucket, final-ten-second display, p50/p95/p99 capture-to-decode, Provider future hit ratio, Interest work per decoded frame, pending state, validation failures, and exact configuration.
