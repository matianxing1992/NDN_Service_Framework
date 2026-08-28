# Tasks: iTiger Qwen3.6 Three-Node Generation

## Phase 1: Frozen contracts

- [x] T001 Freeze the user-selected three-node RTX 5000 layout, 64-token limit,
  five prompts, one warmup plus five measured repetitions, complete metrics,
  official Qwen3.6-27B revision, and no-live-action boundary in
  `specs/162-itiger-qwen36-generation/`.

## Phase 2: Foundational runtime compatibility

- [x] T002 Add failing unit and contract tests, implement an additive
  `qwen3_5` hybrid-layer stage loader/executor with strict state-dict mapping,
  non-thinking text-only formatting, runtime-version checks, and CUDA
  fail-closed behavior, preserve the `qwen2` path, and close with focused tests
  in `examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py`,
  `tests/python/test_spec162_qwen36_stage.py`, and runtime packaging manifests.

- [x] T003 Build and seal one coherent Qwen3.6-capable OCI/SIF runtime, then
  pass the tiny local Docker and single-node RTX 5000 SIF operation-status
  collaboration gates without Qwen weights or runtime overrides, retaining
  release/evidence manifests under `specs/162-itiger-qwen36-generation/evidence/`.

## Phase 3: Complete distributed answer (User Story 1)

### Historical hard architecture gate

The pre-authorization pause prohibited TigerCluster mutation or execution
while `specs/163-di-collaboration-planning` was open. That rule remains the
historical boundary for unrelated work, but the user explicitly authorized
the current T009 small-Qwen requalification sequence. Its jobs, source
bundles, and evidence still require fresh immutable identities and exactly-once
recording; existing failed identities remain immutable.

- [x] T004 [US1] Implement and locally validate RTX-only reference preparation,
  streaming stage construction for `[0,21)`, `[21,42)`, `[42,64)`, strict CUDA
  load/peak-memory evidence, immutable prompt/reference contracts, explicit
  reference placement matching those same stage boundaries, and no-download
  capacity gating in `specs/162-itiger-qwen36-generation/jobs/`,
  `tests/python/test_spec162_qwen36_stage.py`, and
  `specs/162-itiger-qwen36-generation/evidence/preflight.md`.

  The no-download 3xRTX 5000 scratch probe passed as Job 174610, but the
  generated capacity decision is intentionally `allowed=false` because no
  authoritative `/project` user quota is available. That was the historical
  T004 blocker; the current controlling blocker is the Spec 163 architecture
  dependency and T009 requalification.

- [ ] T005 [US1] After explicit authorization, execute exactly one RTX-only
  preparation allocation and one linked three-node, one-prompt development
  smoke; require EOS within 64 tokens, exact reference match per token, three
  CUDA stage receipts and two dependency receipts per epoch, and preserve all
  evidence under the frozen identity.

  The authorized first preparation identity ran as Job 175053 and is preserved
  as `FAILED 1:0` after 39 seconds because the container omitted
  `/opt/ndnsf-app/python` from `PYTHONPATH`. It downloaded no model and promoted
  no artifact. The local fix passes the exact OCI non-root import gate, but a
  new live identity requires new explicit authorization; the linked smoke
  remains unsubmitted. That corrected pre-architecture replacement is now
  `RETIRED_UNSUBMITTED` and cannot be submitted after the architecture pause.

## Phase 4: Repeated distributions (User Story 2)

- [ ] T006 [US2] Implement the five-prompt campaign/analyzer contract with one
  retained warmup and five sequential measured generations per prompt,
  full-answer/token evidence, TTFT, inter-token, total-latency, output-length,
  tokens/s, exclusion accounting, per-prompt summaries, and pooled descriptive
  summaries in `specs/162-itiger-qwen36-generation/jobs/` and focused tests.

- [ ] T007 [US2] After separate explicit authorization and T005 PASS, submit
  the formal campaign exactly once, preserve its first terminal result, and
  publish an evidence-backed report without p99, scaling, KV-cache, MTP, or
  production claims.

## Phase 5: RTX-only reproducibility (User Story 3)

- [ ] T008 [US3] Audit the final manifests, Slurm records, node/GPU mapping,
  cleanup inventory, checksums, and documentation to prove no H100 dependency,
  no CPU fallback, no login-node compute, and no mutation of Spec 161 Job
  174382 in `specs/162-itiger-qwen36-generation/traceability.md` and maintained
  bilingual/operator documentation.

## Phase 6: Architecture requalification prerequisite (User Story 1)

- [ ] T009 [US1] After Spec 163 passes its architecture, implementation,
  security, local Docker, and MiniNDN gates, requalify Spec 162 with a fresh
  source/OCI/SIF identity and evidence schema; repeat local import/fake
  collaboration gates, refresh bounded capacity evidence, and register the
  exact `[0,21)`, `[21,42)`, `[42,64)` artifacts as a pre-split manifest
  selected by the built-in pre-split-first strategy. T009 performs no live
  model preparation without a new explicit authorization.

  Local progress (2026-07-29): the Qwen3.6 placement adapter, deferred
  ACK-closed planning path, post-Selection GPU preparation, terminal
  reservation release with persistent GPU residency, formal 5 × (1 warmup + 5
  measured) campaign, cold/warm analyzer, and bounded
  `DistributedRepo.put_file()`/`get_file()` path have focused test coverage.
  A fresh layered application image and Qwen overlay pass runtime, package,
  banned-path, and local fake-collaboration gates. A small content-bound
  instrumentation overlay adds explicit ACK=true evidence, response-wait
  timing, complete-generation cold/warm separation, and the invariant
  `release(request N) < ACK=true(request N+1)`. Its exact parent, local image,
  source seal, source bundle, and Docker archive identities are recorded in
  `evidence/t009-requalification.md`. The older Tiger-staged candidate is
  retained as superseded. The current `2b4e...` source bundle and complete
  archive are staged read-only on Tiger and pass remote size, full SHA-256,
  gzip, single-image manifest/config, source-seal, and no-write-bit gates.
  A separate T009 renderer now requires a
  byte-digest-matched single-image OCI manifest with the exact frozen config
  image ID before emitting candidate-bound materialization and GPU-smoke jobs;
  its five unit tests and shell syntax gates pass. The historical
  `6216ef2b9525...` jobs remain frozen. The legacy `put()` whole-object path
  remains forbidden for these stages. The renderer snapshot inside the staged
  measurement bundle is archival and must not be run; publication rendering
  uses the current repository copy and stages a separately hashed plan. The
  formal analysis contract is now v3: it fail-closes on missing per-Request
  timing fields, summarizes every requester and Provider stage for cold and
  warm paths, and proves
  `ACK_TRUE < STAGE_TIMING < RELEASE < NEXT_ACK_TRUE`. Its analyzer and
  contract are separately sealed so the frozen measurement bundle is not
  mutated.

  Remaining: obtain an immutable OCI manifest digest, materialize and verify a
  fresh SIF, run bounded Repo-node publication/fetch, obtain real root-manifest
  receipts for all three stages, and verify the exact provider fetch/load and
  warm GPU-reuse path. Registry publication and every Slurm submission still
  require explicit authorization. Until the Repo receipts exist, the generated
  catalog remains `REQUIRES_DISTRIBUTED_REPO_REGISTRATION`.

  T009 was added after the historical T005–T008 identities were assigned, so
  its dependency position intentionally precedes any new T005 attempt even
  though it is listed after the historical tasks.

## Dependencies

```text
T001 -> T002 -> T003 -> T004
     -> Spec 163 PASS
     -> T009 requalification
     -> new explicit authorization
     -> T005 -> T006 -> T007 -> T008
```

T005 and T007 are separate exactly-once authorization boundaries. No live task
is implied by completion of local implementation tasks.

## Validation

- T002 closes local unit/contract compatibility before any runtime build.
- T003 closes image/binding behavior without loading model weights.
- T004 closes implementation and no-download capacity behavior. Real stage
  fit, CUDA forward, and artifact promotion were not performed.
- T009 requalifies the changed architecture and runtime before any new live
  preparation identity.
- T005 closes one complete distributed answer before repeated measurement.
- T006 closes analyzer reproducibility before T007 submission.
- T008 is the final evidence, cleanup, documentation, and claim-boundary audit.

## MVP

T001 through T005: one exact EOS-terminated Qwen3.6 answer across three physical
RTX 5000 nodes. The repeated campaign begins only after that capability passes.
