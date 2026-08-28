# Spec 168 v34 Candidate Audit

## Verdict

**PASS Gate D; authorize exactly one Gate E three-node Qwen3-0.6B control.**
This verdict does not claim that Gate E or T008 has passed. Automatic retry is
forbidden; any runtime or wrapper failure closes this campaign identity.

## Frozen identity

- Campaign: `spec168-campaign-v3-fa3112471039fabb3f03`.
- Source: `sha256:e090406547cd36ea426b7d6c63784741f60ddb6d2eea3449e7e8e60837686d4a`.
- Source bundle: `sha256:fe70b185eba5384143c0f9ce240aea0e4f4ec8712f01324033e83ddf3a955133`.
- Exact SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Small stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
- Gate C result: `sha256:2b96645315de1663adc2adf0eb39c53b8981e7f43d95ff490f53302fedf50deb`.
- Route/launch input: `sha256:3c7aa4de9a169c9d25b5bcb1b9869560e3836786c1ee0c0f3dd7400627a73ea3`.
- Analyzer: `sha256:953263123b2f4078ee36768c6de5dcf64a774cc33d1d6563b0540f3d47dcf208`.
- Gate E schedule input: `sha256:9c78732f650bec5dad47ba5eae2988637e22da3c0089cbd035a4c41d812ca3f6`.

## Gate evidence

- Spec Kit strict structure: PASS, with 24 functional requirements, 11
  success criteria, 4 user stories, and 17 tasks. The absent final
  traceability report belongs to T017 and is not a T008 admission artifact.
- Spec Kit prerequisites: PASS.
- Focused local contracts: 68 tests pass across Selection/dataflow, deferred
  planning, Provider generation, real-MiniNDN admission contracts, campaign,
  exact-SIF/source bundle, three-node analyzer, cache residency, and lifecycle
  reconciliation.
- Local exact-image overlay: PASS under 2 GiB memory and 2 GiB memory-plus-swap.
- Gate B remains the v30 real-MiniNDN PASS under 6 GiB memory and 7 GiB
  memory-plus-swap. v34 changes only deployment/cleanup/analysis surfaces and
  does not require local full-model materialization.
- Gate C job 182386: `COMPLETED 0:0` in 1:49 on one RTX 5000. Complete package
  copy-up, native extensions, three CUDA stages, top-token reference, and
  cleanup all passed with zero CPU fallback.
- Negative lineage job 182385 is retained: CUDA passed but its batch failed
  during host-side deletion of a read-only overlay. v34 moves cleanup inside
  the container; no inference behavior changed.

## Closure of v30 blockers

- D-001 closed: `gate-e-small-single.sbatch`,
  `spec168-three-node-rank.sh`, and
  `spec168-three-node-rank-inner.sh` form a native Spec 168 launch surface.
  The measured Spec 162 process/route bootstrap is behind an explicit adapter,
  not silently promoted as Spec 168 evidence. There is no fixed Provider/User
  settle delay.
- D-002 closed: the route/launch, analyzer, schedule, prompt set, source bundle,
  SIF, and stage manifest are executable file-backed digests in one frozen V3
  campaign.
- D-003 closed: `spec168-three-node-analyzer.py` reconciles all three node/GPU,
  face/route, Provider, request/ACK/plan, dependency, CUDA, security, token,
  and terminal Response observations. It fails on request-ID drift, per-token
  Requests, CPU fallback, legacy ready-set activation, retained secrets, or a
  successful User hidden inside a failed wrapper.

## Authorized action

Upload the payload-free campaign directory, submit
`gate-e-small-single.sbatch` once with request ID
`spec168-fa3112471039fabb3f03-single`, and retain its terminal evidence whether
it passes or fails. Do not rebuild the SIF, prepare another model, copy model
payloads into the run directory, or resubmit this campaign identity.

## Five-tool gate report

- Context Mode: stable and active-Spec health passed earlier; repository files
  remain authority.
- CodeGraph: used before source/runtime changes and synchronized.
- Spec Kit: strict structure, prerequisites, campaign validation, and this
  code-aware Gate D audit pass.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable to this implementation admission; no literature,
  statistical, or comparative research claim is made.
