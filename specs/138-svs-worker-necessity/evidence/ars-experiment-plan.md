# ARS Code Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-23T00:00:00-05:00
- Verification Status: UNVERIFIED
- Version Label: code_plan_v1

## Experiment Overview

- **Title**: Same-binary single-worker Sync-production necessity
- **Objective**: Determine whether asynchronously moving serial NDN-SVS Sync
  production from the Face thread to exactly one worker is necessary to protect
  Face responsiveness at a valid, preregistered pressure boundary.
- **Hypothesis**: The worker will remove at least half of production CPU from
  Face and reduce heartbeat p99 by at least 20% in two of three pairs without
  material delivery harm.
- **Type**: systems benchmark

## Variables

- Independent variable: runtime production mode (`face-serial` or
  `worker-serial`).
- Primary dependent variables: Face production CPU and Face heartbeat p99.
- User-visible dependent variables: attempted-rate fidelity, delivery ratio,
  and delivery p99.
- Controlled variables: source, binary, publication API, signing algorithms,
  protocol, topology, payload, publisher/receiver roles, CPU map, timing,
  diagnostics, and formal order.
- Potential confounds: host CPU contention, warming, order drift, NFD
  contention, caller-side publication cost, worker queueing, and stale results.

## Setup

- **Language/Framework**: Existing C++17 subject; Python 3 orchestration;
  MiniNDN
- **Working Directory**:
  `/home/tianxing/NDN/ndn-service-framework`
- **Dependencies**: Current MiniNDN/NFD, exact NDN-SVS subject, ndn-cxx, Boost
  1.71
- **Environment**: Four-vCPU Ubuntu host; one worker thread only

## Inputs

| Input | Path | Description |
|---|---|---|
| Subject manifest | `build/spec137-four-core/source-manifest.json` | Frozen source/binary/linkage identity |
| Benchmark binary | `build/spec137-four-core/bin/svs-serial-production-offload` | One binary containing both runtime modes |
| Contract | `specs/138-svs-worker-necessity/contracts/experiment-contract.md` | Preregistered gates and decision |

## Expected Outputs

| Output | Path | Format | Success Criterion |
|---|---|---|---|
| Calibration | `results/spec138-svs-worker-necessity/<id>/calibration/` | JSON/raw logs | One rate selected without reading worker outcomes, or explicit no-pressure result |
| Formal receipts | `results/spec138-svs-worker-necessity/<id>/receipts/` | JSON | Exactly six unique once-only receipts |
| Run metrics | `analysis/run-metrics.csv` | CSV | All six cells present |
| Paired effects | `analysis/paired-contrasts.csv` | CSV | Three run-level pairs |
| Decision | `analysis/conclusion.json` | JSON | Deterministic registered taxonomy |
| Report | `evidence/worker-necessity-report.md` | Markdown | Includes all outcomes and excluded claims |

## Monitoring Configuration

- **Timeout**: 120 seconds per formal cell plus bounded setup
- **Monitor files**: cell stdout/stderr, event JSONL, resource JSON, receipt
  directory
- **Metric file**: per-cell event JSONL and offline run metrics

## Analysis Plan

- Experimental unit: one complete run.
- Replicates: three paired runs per mode.
- No packet-level p-values.
- Primary criterion: the FR-014 conjunction; no post-hoc threshold changes.
- Order control: AB/BA/AB.
- Rate selection: control-only descending preregistered ladder.
- Negative, contaminated, and inadmissible cells remain in the report.
- Statistical fallacy controls: preregistration limits look-elsewhere and
  forking paths; all terminal cells prevent survivorship bias; paired
  run-level analysis avoids pseudoreplication; causal language is restricted
  to the randomized runtime treatment at the tested boundary.
