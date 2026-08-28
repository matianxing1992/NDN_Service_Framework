# Implementation Plan: Data-Centric Authorization Evaluation

**Branch**: `spec170-wip` | **Date**: 2026-08-11 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/172-data-centric-authorization-evaluation/spec.md`

## Summary

Revise the NDNSF paper around two connected systems contributions: a uniform,
producer-scoped Data transaction for Request, ACK, Selection, and Response; and
a separation between signer authentication and service-semantic authorization.
Add reproducible experiments for authorization correctness, Provider-decoupled
User onboarding, and cold/warm authorization overhead. Paper placeholders remain
explicitly `TBD` until retained experiment evidence passes the claim gate.

The implementation proceeds from the current runtime rather than inventing a
new security path. Existing signature validation, service/permission attribute
routing, encrypted PermissionResponse, provider authorization tables, policy
epochs, and one-time token checks are the subjects under test. Protocol changes
are permitted only when an experiment exposes a gap that prevents the stated
bounded contribution, most notably policy refresh after a User-only policy
change.

## Technical Context

**Language/Version**: C++17 runtime and tests; Python 3 experiment analysis; Bash regression orchestration; LaTeX paper

**Primary Dependencies**: ndn-cxx, NDN-SVS Experimental, NAC-ABE/OpenABE, Boost.Test, MiniNDN for final network validation

**Storage**: Tracked specifications and paper sources; local canonical result directories with JSON/CSV manifests and hashes

**Testing**: Focused Boost unit tests, existing security regression scripts, new composed authorization integration runner, pytest for manifest/result validation, MiniNDN final path when network behavior is in scope

**Target Platform**: Linux development host for deterministic focused tests; MiniNDN Linux environment for final network/security evidence

**Project Type**: C++ service framework plus experiment harness and academic paper

**Performance Goals**: Quantify rather than prescribe cold/warm overhead; retain median, p95, maximum, operation counts, wire bytes, and failures at each registered scale point

**Constraints**: Preserve existing dirty work; do not weaken any security gate; no numerical paper claim without canonical evidence; do not conflate Provider-local reconfiguration with automatic policy refresh

**Scale/Scope**: Correctness matrix plus onboarding repetitions; initial overhead points cover 1/10/100 Users, 1/4/16 Providers, and multiple service-policy sizes where the environment can complete reliably

## Constitution Check

- **Canonical Dynamic Runtime**: PASS. All experiments use the generic V2
  Request/ACK/Selection/Response path and unified service names.
- **Security Is Part Of The Data Path**: PASS. The feature evaluates existing
  NAC-ABE, permission, token, epoch, replay, and provider-permission checks. No
  bypass is allowed even for ablation runs; unsafe comparisons must be isolated
  test subjects that cannot be selected by production configuration.
- **CodeGraph First**: PASS. Current attribute routing, permission distribution,
  epoch validation, and token paths were traced before planning.
- **Spec-Driven Changes**: PASS. Spec 172 owns the paper/evaluation contract;
  Spec 171 remains the mobility evidence source.
- **Verify With The Right Scope**: PASS. Deterministic logic starts with focused
  tests and existing regressions; composed network claims require MiniNDN.
- **Cohesive Tasks**: PASS. Tasks are organized by contribution framing,
  composed authorization correctness, onboarding semantics, and measured cost.
  Tests, implementation, evidence, and paper replacement for one behavior stay
  in the same task.

Post-design re-check: PASS. The contracts below do not weaken runtime security,
introduce legacy APIs, or treat local result directories as authority without a
tracked evidence summary.

## Research Decisions

See [research.md](research.md). Controlling decisions are:

1. Use `data-centric service transaction`, not `data-driven`, and avoid a
   standalone “first use of Data” claim.
2. Frame novelty as framework integration of producer-scoped Data, dynamic
   multi-Provider selection, and service-semantic authorization.
3. Treat NAC-ABE as an adopted primitive; compare authorization architecture,
   not cryptographic novelty.
4. Use same-framework matched ablations for causal overhead; use MF-IoT, DNMP,
   NSC, and NAC/NAC-ABE as primary-source architectural comparisons.
5. Report “no per-User Provider configuration” separately from policy refresh.
6. Keep unmeasured paper cells as labeled `TBD`.

## Design

### Paper revision boundary

The abstract and introduction state the two contributions. Background separates
the adopted primitives from the framework contribution. Design introduces a
three-layer security model and a four-message policy table. Related work compares
authorization authority and transaction structure. Evaluation registers the
three security research questions and contains `TBD` tables. Conclusion repeats
only claims supported by current evidence or explicitly identifies planned work.

### Experiment layers

1. **Deterministic component layer**: extend focused tests for attribute routing,
   encrypted permission responses, authorization tables, policy epoch rejection,
   signer/name validation, and token replay.
2. **Composed local integration layer**: run Controller, User, and Provider through
   the real security path; prove expected terminal state and handler count for
   every registered case.
3. **Onboarding layer**: freeze Provider-local hashes, provision a new User while
   the Provider is unavailable, then distinguish unchanged configuration from
   required controller-policy refresh on reconnection.
4. **Overhead layer**: run matched cold/warm subjects, use runtime crypto counters,
   and record latency and wire/state cost without changing the service workload.
5. **MiniNDN confirmation layer**: repeat the composed authorized/denied and
   onboarding cases over the normal NDNSF network path when the local contract is
   stable.

### Claim gate

Every claim has a stable identifier in
`contracts/claim-evidence-matrix.md`. A claim can move from `PLANNED` to
`SUPPORTED` only when its registered evidence files exist, hashes match the
manifest, the subject reached a terminal state, and the required reproduction
count passed. A paper table may contain `TBD`, but prose must call it planned and
must not infer direction or magnitude.

## Project Structure

### Documentation

```text
specs/172-data-centric-authorization-evaluation/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── authorization-cases.yaml
│   ├── claim-evidence-matrix.md
│   └── experiment-manifest.schema.json
├── checklists/requirements.md
└── tasks.md
```

### Implementation and paper files

```text
docs/PAPER/named-data-network-service-framework-paper/
├── NDNSF.tex
└── sections/
    ├── abstract.tex
    ├── introduction.tex
    ├── background.tex
    ├── design.tex
    ├── relatedWork.tex
    ├── evaluation.tex
    ├── conclusion.tex
    └── references.bib

ndn-service-framework/
├── ServiceController.cpp
├── ServiceProvider.cpp
├── ServiceUser.cpp
├── common.hpp
└── utils.cpp

tests/unit-tests/
├── encrypted-permission-response.t.cpp
├── generic-dynamic-api-crypto-auth.t.cpp
├── generic-dynamic-api-tokens-replay.t.cpp
└── service-authorization-table.t.cpp

examples/
├── run_security_regressions.sh
├── run_nac_abe_attribute_routing_regression.sh
└── run_token_handshake_negative_regression.sh

Experiments/
├── run_authorization_evaluation.py
└── analyze_authorization_evaluation.py

tests/python/
└── test_authorization_evaluation.py
```

**Structure Decision**: Extend the existing paper, test, example, and experiment
locations. Do not create a parallel security implementation or a second paper
tree.

## Verification Strategy

- Validate Spec artifacts and task cohesion before implementation.
- Run the focused security unit-test cases affected by each behavior.
- Run the existing security regression suite after composed-path changes.
- Run the new experiment first as deterministic smoke, then formal repetitions.
- Verify manifests and result hashes independently of paper generation.
- Build `NDNSF.tex`, inspect warnings/errors, and run a claim scan that rejects
  unsupported “first”, zero-interaction, trust-chain-elimination, or revocation
  wording.
- Replace `TBD` only after evidence is canonical and re-run the paper build.

## Complexity Tracking

No constitution violations are accepted. The same-framework ablation is retained
because comparing full NDNSF with a different service protocol would confound the
authorization cost; it must remain test-only and cannot disable production gates.
