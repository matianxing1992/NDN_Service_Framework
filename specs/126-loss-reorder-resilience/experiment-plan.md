# Code Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-19
- Verification Status: UNVERIFIED
- Version Label: spec126_frozen_matrix_v1

## Experiment Overview

- **Title**: Adaptive sample-atomic loss and reorder boundary
- **Objective**: Determine whether the upgraded existing recovery/reorder state
  machine preserves live correctness and bounded work under isolated 1% loss,
  bounded reordering, and their combination.
- **Hypothesis**: Deterministic invariants pass; isolated reorder completes 5/5;
  isolated loss and combined profiles complete at least 4/5 without lifecycle
  failure or final-ten-second stalls longer than one second.
- **Type**: environment-sensitive network simulation

## Variables

- Independent: loss `{0,1}%`; reorder `{off,on}` with the frozen profile.
- Primary dependent: admissible 60-second GUI completion without lifecycle
  failure or final-ten-second stall over one second.
- Secondary: decoded frames/gaps, retry/recovery/deadline/late counts, Interest
  overhead, future-hit ratio, p50/p95/p99 latency, qdisc counters.
- Held constant: source revision, topology endpoints/bandwidth, workload,
  duration, start delay, encoder, bitrate, width, FEC, policy, logging, sampler,
  NFD level, and launcher.
- Confounds recorded: unseeded netem RNG, shared-host scheduling, dummy MiniNDN
  key-chain patch, validation/decoder CPU, process crashes.

## Setup

- **Language/Framework**: Python 3, C++17, MiniNDN, Linux netem
- **Entry Command**: frozen by the generated campaign manifest before execution
- **Working Directory**: `/home/tianxing/NDN/ndn-service-framework`
- **Dependencies**: current build, MiniNDN/NFD, GStreamer, `tc`, xvfb, sudo
- **Environment**: local Linux VM; timing is environment-sensitive

## Expected Outputs

| Output | Path | Format | Success Criterion |
|---|---|---|---|
| manifest | `results/spec126-*/campaign-manifest.json` | JSON | 16 unique commands, frozen source/profile |
| runs | `results/spec126-*/runs/*` | logs/JSON/CSV | one directory per invocation |
| summary | `results/spec126-*/campaign-summary.json` | JSON | all cells/counters/verdicts present |
| table | `results/spec126-*/campaign-runs.csv` | CSV | 16 rows, no duplicate command ID |
| hashes | `results/spec126-*/evidence-hashes.json` | JSON | Spec 125 before/after identical |

## Monitoring Configuration

- **Timeout**: 210 seconds per 60-second run
- **Monitor files**: launcher, controller, drone, ground-station, qdisc, PIT,
  timeline, and acceptance summary artifacts in the active run directory
- **Experiment type override**: environment-sensitive
- **Metric file**: per-run acceptance summary plus campaign CSV/JSON
- **Metric key**: `accepted`

## Analysis Plan

- Primary: completion count per cell with exact two-sided 95% Clopper-Pearson
  interval; accept using the preregistered 5/5 and 4/5 engineering thresholds.
- Secondary: report distributions and paired configuration deltas descriptively;
  no p-value or improvement claim with this sample size.
- A run failure is data. Never auto-retry, replace, or exclude it because its
  result is unfavorable.
- Do not compare wall-clock timing across hardware; within this frozen shared
  host, report latency distributions with the environment caveat.
