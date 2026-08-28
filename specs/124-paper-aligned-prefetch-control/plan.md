# Implementation Plan: Paper-Aligned Prefetch Control

**Branch**: `Experimental` | **Date**: 2026-07-19 | **Spec**: [spec.md](spec.md)

## Summary

Correct the remaining mismatch between NDNSF's mapped exact-name prefetch controller and Gusev et al.: preserve the detection hold, restore the previous usable pipeline after over-adjustment, and estimate network retrieval delay without treating future production wait as RTT. Keep wire names, security, FEC, and public APIs unchanged, then verify with deterministic unit traces and a fresh UAV MiniNDN run.

## Technical Context

**Language/Version**: C++17 and Python 3

**Dependencies**: ndn-cxx, NFD/MiniNDN, existing NDNSF Stream and UAV runtime

**Testing**: Boost.Test Stream/UAV unit suites and the existing 60-second MiniNDN UAV harness

**Constraints**: bounded state; no wire/API/security change; `NDN_LOG` only; preserve prior evidence

## Constitution Check

- CodeGraph-first code tracing: PASS.
- Spec-driven protocol/controller repair: PASS.
- Security and semantic naming preservation: PASS.
- MiniNDN final network validation: PASS.
- Cohesive task granularity: PASS; three independently reviewable outcomes.

## Design

### Detection epoch and previous usable window

The controller records an action time and the pipeline value that was usable immediately before each withhold. A phase/window mutation is eligible only after the configured hold. When withholding first makes arrivals unstable, restore that saved value and enter `Fetching`; do not return to `Chasing` and double again. `holdMs` uses the caller's current monotonic decision time.

### Effective delay versus network delay

For known-produced Data, exact Interest-expression-to-Data reception remains a direct network observation. For an Interest expressed ahead of the join checkpoint, the same interval is effective delay (`network + production wait`) and cannot raise network RTT during `Chasing` or `Adjusting`; it may only lower an existing overestimate. Once `Fetching` is stable, the paper states that effective delay is close to network delay, so normal two-direction adaptation resumes. This needs no new packet metadata or observation buffer.

The consumer-side counter retains source compatibility but is described as `ahead of join checkpoint`, because the immutable initial checkpoint cannot prove Provider state at every later expression. Provider counters remain authoritative for actual pending-future admission and hits.

### Evidence

Add failing deterministic controller/delay tests before implementation. After focused and relevant suites pass, run one uniquely named 60-second zero-loss MiniNDN experiment under the existing Spec 123 original-load contract and retain the result regardless of outcome.

## Project Structure

```text
ndn-service-framework/Stream.{hpp,cpp}
tests/unit-tests/stream.t.cpp
Experiments/NDNSF_UAV_GUI_Minindn.py
Experiments/analyze_stream_latency.py
specs/124-paper-aligned-prefetch-control/
results/spec124-*/
```

No new external contract or wire entity is introduced.
