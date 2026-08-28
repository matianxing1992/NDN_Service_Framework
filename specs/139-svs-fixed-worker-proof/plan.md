# Implementation Plan: Fixed-Rate Single-Worker Proof

**Branch**: `139-svs-fixed-worker-proof` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

## Summary

Reuse the exact Spec 138 mechanism code and frozen binary, but create a new
fixed 600 pps qualification and formal authority. Do not alter NDN-SVS, the
benchmark, or predecessor evidence.

## Technical Context

**Language/Version**: Python 3.8+; existing C++17 binary  
**Dependencies**: MiniNDN/NFD, NDN-SVS, ndn-cxx, Boost 1.71  
**Testing**: Python unittest, real two-node MiniNDN, offline hash verifier  
**Platform**: Existing four-vCPU Ubuntu host  
**Performance goal**: Valid 600 pps in both modes; 60-second formal windows  
**Constraints**: One binary, two modes, one worker, no retry  
**Scope**: Two qualification cells and six formal cells

## Constitution Check

- Intent fidelity: PASS; this is the requested minimal comparison.
- Code reality: PASS; verified existing runtime switch and one-worker path.
- MiniNDN: PASS; final evidence uses two independent nodes/processes.
- Frozen evidence: PASS; Specs 137/138 are protected inputs.
- Task cohesion: PASS; audit, qualification, formal execution, and closure are
  independent gates.

## Experiment Design

```text
qualification:
  face-serial @ 600 pps, 5/15/5
  worker-serial @ 600 pps, 5/15/5

formal:
  01 face, 02 worker, 03 worker, 04 face, 05 face, 06 worker
  each 10/60/10
```

All mode-independent controls and the binary are identical. The existing
Spec 138 analyzer metrics and classification predicates are reused under a new
Spec 139 schema and hash authority.

## Project Structure

```text
Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py
Experiments/analyze_svs_fixed_worker_proof.py
tests/python/test_spec139_svs_fixed_worker_proof.py
specs/139-svs-fixed-worker-proof/
results/spec139-svs-fixed-worker-proof/<campaign-id>/
```

## Complexity Tracking

No new NDN-SVS mechanism is introduced.
