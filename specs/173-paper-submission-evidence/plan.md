# Implementation Plan: Submission-Ready NDNSF Evidence

**Branch**: `spec170-wip` | **Date**: 2026-08-11 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/173-paper-submission-evidence/spec.md`

## Summary

Converge the NDNSF manuscript and its evidence package so every retained number is reproducible, the May 20 scientific core remains visible, and novelty claims stay bounded by implementation and literature evidence. Recover retained artifacts first; replace only the high-value missing comparisons with a registered, compact MiniNDN campaign; remove unverifiable auxiliary precision; then rebuild and audit the ten-page body.

## Technical Context

**Language/Version**: Python 3 experiment/analyzer scripts; C++17 NDNSF examples/runtime; LaTeX manuscript

**Primary Dependencies**: MiniNDN, ndn-cxx, NDN-SVS Experimental, NAC-ABE, NSC, Python gRPC, IEEEtran

**Storage**: Canonical JSON/YAML/CSV/Markdown evidence under the Spec; raw campaign output under `results/`; PDF/LaTeX under the paper directory

**Testing**: Python compile/dry-run tests, existing C++ unit and regression gates, MiniNDN smoke and confirmatory runs, deterministic artifact audit, LaTeX build and rendered-page inspection

**Target Platform**: Current Ubuntu development VM with the system NDNSF/NDN-SVS toolchain; final paper is platform-neutral PDF

**Project Type**: C++ service framework plus Python experiment harnesses and academic manuscript

**Performance Goals**: Issue at least 80% of the registered scheduled requests in non-admission cells or invalidate the matched block; retain scheduled, issued, admitted, completed, and timed-out outcomes; report per-repetition variation; do not optimize toward a desired ranking

**Constraints**: Ten body pages excluding references; 60-second measured performance windows; no host-NFD final evidence; Sync suppression remains 1--5 ms; matched baseline conditions; preserve unrelated dirty-worktree changes; no new broad mobility matrix

**Scale/Scope**: One compact replacement campaign (three systems, two one-Provider rates, paired admission cells, one custom-selection rate), existing authorization/mobility/work-efficiency evidence, one artifact index, and the ten-page manuscript

## Constitution Check

*GATE: Passed before Phase 0 and re-checked after design.*

- **Canonical runtime**: PASS. Replacement experiments use the current dynamic runtime and unified service names; no generated/static API is reintroduced.
- **Security in the data path**: PASS. Performance runs retain the normal authorization/token path unless a registered control explicitly states otherwise; no bypass is permitted.
- **CodeGraph first**: PASS. Current experiment entry points and runtime behavior were located with CodeGraph before detailed source inspection.
- **Spec-driven durable work**: PASS. This Spec owns the experiment registration, evidence contracts, manuscript convergence, and acceptance gates.
- **Right-scope verification**: PASS. Network/performance evidence uses MiniNDN, performance windows are 60 seconds, and smoke runs are excluded from claims.
- **Cohesive tasks**: PASS. Future tasks are organized by complete evidence outcomes rather than file-by-file edits.

The plan identifies cohesive behavioral slices and their acceptance gates. It does not prescribe one future task per file, test command, or evidence document.

## Project Structure

### Documentation (this feature)

```text
specs/173-paper-submission-evidence/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── experiment-registration.yaml
│   ├── artifact-index.schema.json
│   └── core-content-ledger.md
├── evidence/            # Canonical manifests, summaries, analysis, and hashes
└── tasks.md
```

### Source Code (repository root)

```text
Experiments/
├── NDNSF_NewAPI_Minindn_Perf.py
├── gRPC_memphis_ucla_latency.py
├── NSC_memphis_ucla_latency.py
└── paper_submission_campaign.py       # matched orchestration and manifests

scripts/
└── analyze_paper_submission_evidence.py

docs/PAPER/named-data-network-service-framework-paper/
├── NDNSF.tex
├── sections/
├── NDNSF.pdf
└── SUBMISSION_AUDIT.md

examples/
└── run_selective_ack_custom_selection_regression.sh
```

**Structure Decision**: Keep the current experiment harnesses as system-specific executors, add one thin campaign/analysis layer for matched repetitions and provenance, store durable summaries under the Spec, and edit only the existing manuscript source. Raw logs remain local experiment output rather than repository authority.

## Complexity Tracking

No constitution violations require justification.
