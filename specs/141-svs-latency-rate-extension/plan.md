# Implementation Plan: SVS Latency Rate Extension

**Branch**: `141-svs-latency-rate-extension` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

## Summary

Create a thin runner and analyzer around the frozen Spec 140 mechanisms. Reuse
the exact binary and `run_cell` implementation; change only the rate matrix and
the independent result namespace.

## Technical Context

**Runtime**: Python 3.8+, existing C++17 binary  
**Network**: MiniNDN/NFD, two nodes  
**Matrix**: four once-only cells  
**Timing**: 10/60/10 per cell  
**Output**: `results/spec141-svs-latency-rate-extension/<campaign-id>/`

## Experiment Matrix

```text
01 face-inline-rsa @ 600 pps/peer
02 worker-rsa      @ 600 pps/peer
03 face-inline-rsa @ 800 pps/peer
04 worker-rsa      @ 800 pps/peer
```

The thin runner imports the existing Spec 140/Base runner functions, passes the
same schema and raw-sample option, and changes only `experiment_namespace` and
rate. The analyzer accepts both `COMPLETE` and `LOAD_UNSUSTAINED`, but rejects
process/harness/security/accounting failures.

## Project Structure

```text
Experiments/NDN_SVS_Latency_Rate_Extension_Minindn.py
Experiments/analyze_svs_latency_rate_extension.py
tests/python/test_spec141_svs_latency_rate_extension.py
specs/141-svs-latency-rate-extension/
results/spec141-svs-latency-rate-extension/<campaign-id>/
```

## Constitution Check

- Same binary/config causality: PASS
- MiniNDN and 60-second measurement: PASS
- Frozen predecessor protection: PASS
- Negative-result retention: PASS
- No NDN-SVS or NDNSF behavior change: PASS
