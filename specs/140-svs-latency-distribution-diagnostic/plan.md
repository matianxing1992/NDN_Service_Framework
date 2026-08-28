# Implementation Plan: SVS Latency Distribution Diagnostic

**Branch**: `140-svs-latency-distribution-diagnostic` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

## Summary

Add auditable delivery-latency distribution capture to the RSA benchmark,
create an independent two-cell MiniNDN diagnostic, and report mean, p50, p95,
and p99 from raw samples. Spec 136 evidence remains immutable.

## Technical Context

**Language/Version**: C++17; Python 3.8+  
**Dependencies**: MiniNDN/NFD, NDN-SVS, ndn-cxx, Boost 1.71  
**Testing**: Python unittest, C++ build, two-node MiniNDN  
**Platform**: Existing four-vCPU Ubuntu host  
**Performance goal**: 400 attempted pps per peer within +/-2%  
**Timing**: 10 s warmup, 60 s measurement, 10 s drain  
**Scope**: Two fresh cells only; no retry and no formal matrix

## Constitution Check

- Intent fidelity: PASS; the plan adds the four requested statistics.
- Evidence integrity: PASS; old summaries are rejected rather than backfilled.
- Code reality: PASS; `m_deliveryDelay` already contains the exact per-delivery
  measured population but currently emits only p99.
- MiniNDN: PASS; both independent nodes publish and subscribe.
- Statistical validity: PASS; mode percentiles use concatenated samples, not
  arithmetic on peer percentiles.
- Task cohesion: PASS; contract, implementation, qualification, and closure
  are separate gates.

## Design

### Benchmark Evidence

The benchmark accepts a new `--delivery-samples` path. At shutdown it writes a
CSV with one `latencyNs` value per entry in `m_deliveryDelay`, then emits:

```text
deliverySamples
deliveryMeanNs
deliveryP50Ns
deliveryP95Ns
deliveryP99Ns
deliverySamplesSha256   (runner manifest)
```

Percentiles use the existing nearest-rank definition. Mean uses the exact
sample sum divided by count. The offline analyzer recomputes every statistic
from the raw file.

### Experiment

```text
01 face-inline-rsa @ 400 pps/peer, 10/60/10
02 worker-rsa      @ 400 pps/peer, 10/60/10
```

The runner reuses the existing MiniNDN topology and security construction while
selecting a Spec 140 namespace, schema, output root, and sample path. It does
not expose a path that can write to the frozen Spec 136 formal directory.

### Analysis

For each peer:

1. load the retained integer samples;
2. require sample count equals `deliveredMeasured`;
3. recompute mean, p50, p95, and p99;
4. require exact integer-field agreement with the peer summary.

For each mode, concatenate both peers' samples and calculate the distribution
once. Do not average peer percentiles.

## Project Structure

```text
Experiments/ndn-svs-pubsub-benchmark/svs-rsa-single-worker.cpp
Experiments/NDN_SVS_Latency_Distribution_Minindn.py
Experiments/analyze_svs_latency_distribution.py
Experiments/build_svs_latency_distribution.py
tests/python/test_spec140_svs_latency_distribution.py
specs/140-svs-latency-distribution-diagnostic/
results/spec140-svs-latency-distribution/<campaign-id>/
```

## Complexity Tracking

Raw sample retention is necessary because combined percentiles cannot be
derived correctly from per-peer percentiles. At 400 pps for 60 seconds, two
integer columns per cell are small compared with existing logs.
