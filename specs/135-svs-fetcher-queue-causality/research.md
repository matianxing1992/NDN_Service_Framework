# Code Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-23
- Verification Status: UNVERIFIED
- Version Label: code_plan_v1

## Experiment Overview

- **Title**: NDN-SVS Fetcher Queue Causality Diagnostic
- **Objective**: Locate the synchronous NDN-SVS boundary with RSA-2048 Data
  signing, then test whether Fetcher admission width and bounded piggyback
  causally change queue/release/delivery behavior at that boundary.
- **Hypotheses**: H0-RSA establishes the actual boundary and signing cost; H1
  window 40 reduces queue pressure relative to 10; H2 7168 bytes reduces
  fallback pressure relative to 4096; H3 queue/Sync callback pressure co-varies
  with timer misses on the shared I/O thread.
- **Type**: generic network benchmark

## Setup

- **Language/Framework**: C++17, Python 3.8, MiniNDN
- **Entry Command**:
  `sudo -n -E taskset -c 0-3 python3 Experiments/NDN_SVS_Fetcher_Queue_Causality_Minindn.py run --manifest build/spec135/campaign-manifest.json`
- **Working Directory**: `/home/tianxing/NDN/ndn-service-framework`
- **Dependencies**: Existing Spec 133 Boost-1.71 profiled parent, ndn-cxx, NFD,
  MiniNDN
- **Environment**: Linux; CPUs 0 and 2; no GPU

## Inputs

| Input | Path | Description |
|---|---|---|
| Frozen parent | `build/spec133/subject-manifest-io.json` | Spec 133 subject identity |
| Diagnostic contract | `contracts/diagnostic-contract.md` | Fixed factors and claim rules |
| Subject manifest | `build/spec135/subject-manifest.json` | New patch/binary/library hashes |

## Expected Outputs

| Output | Path | Format | Success Criterion |
|---|---|---|---|
| Receipts | `results/spec135-svs-fetcher-queue-causality/confirm01/receipts/` | JSON | Exactly eight terminal receipts |
| Cell evidence | `.../cells/` | JSON/JSONL/logs | Two peers, routes, commands, raw events |
| Summary | `.../causality-summary.csv` | CSV | Sixteen peer rows and all required metrics |
| Report | `specs/135-svs-fetcher-queue-causality/evidence/causality-report.md` | Markdown | Hypothesis verdicts and solution analysis |

## Monitoring Configuration

- **Timeout**: 8 minutes per cell, 70 minutes campaign
- **Monitor files**: terminal receipts, peer stdout/stderr, resource samples
- **Experiment type override**: generic
- **Metric file**: per-cell peer event JSONL and stage summaries
- **Metric key**: terminal receipt status plus attempted/delivered counts

## Analysis Plan

- **Primary metric**: Payload Fetcher queue wait and missed publication releases
- **Success threshold**: The preregistered matched-contrast rules in SC-004 and
  SC-005
- **Comparison**: Five-rate RSA boundary plus two window contrasts and two
  piggyback contrasts at the selected boundary/stress rate
- **Statistical scope**: Descriptive factorial contrasts only; no inferential
  test from `n=1` per combination
