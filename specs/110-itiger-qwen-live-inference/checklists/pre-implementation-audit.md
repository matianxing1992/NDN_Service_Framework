# Pre-implementation Audit R1: NDNSF-DI iTiger Distributed Qwen Execution

**Date**: 2026-07-13
**Mode**: code-aware, reviewer-style pre-implementation audit
**Verdict**: **PASS for implementation after remediation; live execution remains NOT STARTED**

This R1 audit supersedes the earlier PASS. The earlier design made cross-node
placement the first Qwen candidate, compared it to an unmatched local baseline,
and did not close the Slurm process-topology or `sbatch` acknowledgement-loss
windows. Those were material readiness defects. Spec 110 now starts with a
controlled one-node/three-provider/three-GPU candidate, treats multi-node 0.5B
as a separately keyed extension, and defines two matched performance contrasts.

The PASS authorizes implementation of the task plan only. It does not assert
that the SIF, network probe, model exports, GPU candidates, or measurements
exist, and it grants no physical-production authority.

## Findings and remediation

| ID | Severity | Dimension | Original problem | Remediation in R1 | Status |
|---|---|---|---|---|---|
| AUD-R1-001 | High | Experimental validity | The first candidate required multiple nodes, conflating generation/runtime/security/network failures. | US2/FR-010/SC-003 and T065-T084 now require one node, three Provider processes, three GPUs, and one NFD; multi-node is independent T088-T089. | Resolved |
| AUD-R1-002 | High | Baseline validity | A local staged path could not be topology-matched to a multi-node candidate, so “NDNSF overhead” mixed framework and placement effects. | FR-023 and T106-T120 define local-staged vs single-node NDNSF-DI for framework overhead, then single-node vs multi-node NDNSF-DI for placement/network delta. | Resolved |
| AUD-R1-003 | High | Architecture/task executability | The current Slurm adapter has one workload command and no role/rank/GPU/NFD supervisor topology. | New allocation-topology contract and T050-T055/T063 require a frozen process map, one supervisor/NFD per node, `srun` ranks, readiness barriers, process-group teardown, and evidence. | Resolved in design; implementation open |
| AUD-R1-004 | High | At-most-once safety | Recording a receipt only after `sbatch` permits duplicate submission when `sbatch` succeeds but its response is lost. | Operator contract and T016-T017 require durable `INTENT_RECORDED` before `sbatch`, deterministic job lookup keys, `SUBMISSION_UNKNOWN`, `squeue`/`sacct` reconciliation, and no auto-resubmit. | Resolved in design; implementation open |
| AUD-R1-005 | High | Evidence integrity | The prior evidence schema forced every run to have multiple nodes/cross-node edges and lacked placement-conditional execution/failure proof. | Schema v2 requires `placementClass`, process map, submission journal, failure boundary, promotion/authority objects, and conditional single-node/multi-node invariants. | Resolved |
| AUD-R1-006 | Medium | Code reality/migration | `QwenGenerationSession.cpp` accepts only Spec 107 candidate IDs, so a valid Spec 110 candidate would be rejected. | T065/T068 require a versioned Spec107/Spec110 parser with explicit Spec105/109 rejection and migration tests. | Resolved in tasks; implementation open |
| AUD-R1-007 | Medium | Task closure | Performance tasks grouped several model sizes, permitting partial work to be mistaken for task completion. | T112-T117 now give each non-0.5B size an independent performance task and require a per-cell submission ledger. | Resolved |
| AUD-R1-008 | Medium | Network eligibility | Requiring TCP and UDP simultaneously could block a TCP candidate on an unused diagnostic transport. | FR-007/SC-002 and T050-T064 gate only the selected transport (TCP default); unselected transports are diagnostic. | Resolved |
| AUD-R1-009 | Medium | Audit quality | The previous audit reported PASS, 12 SC, and 140 tasks without detecting the defects above. | This R1 report supersedes it, records the code-aware defects, and is bound to 13 SC and 147 tasks. | Resolved |

No unresolved CRITICAL or HIGH document-design finding remains after R1.
External cluster state and the explicitly tasked implementation gaps remain
controlled risks, not completed capabilities.

## Code reality checked

- `OnnxRuntimeModelRunner.cpp` already selects CUDA or CPU, records the selected
  provider/device, and can expose CPU fallback, but GPU PASS still needs the
  allocation/provider/stage/UUID correlation required by T070.
- `QwenGenerationSession.cpp` is currently a three-role session state/ledger,
  not yet the complete one-request generation path, and its candidate regex is
  Spec107-only. T065-T073 own that gap.
- `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` still drives
  acceptance through per-token requests. T072 replaces this with one bounded
  generation request and keeps the old loop diagnostic-only.
- `packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py` can render,
  submit, query, wait, and cancel a basic workload, but it does not yet render
  the complete role/process topology or close the ambiguous-submit window.
- The current Slurm template has one workload command; no existing code justifies
  claiming multi-process/multi-node Qwen execution before T050-T073 complete.

## Architecture and ownership audit

- Slurm remains the sole allocation/lifecycle authority.
- Apptainer runs OCI-derived SIFs; no iTiger Docker daemon or privileged service
  is introduced.
- NFD owns forwarding; NDNSF owns security/invocation; NDNSF-DI owns generation,
  stages, dependencies, provider replacement inside a session, and evidence.
- Exactly one unprivileged NFD is supervised per unique allocated node. Multiple
  same-node Providers share the node-local NFD endpoint.
- A Slurm job is never automatically retried. Bounded in-session provider
  replacement is separately authorized, evidenced, and does not create a new job.
- Immutable OCI/SIF releases retain current and prior rollback targets; accepted
  evidence and identity bindings cannot be overwritten during rollback.
- Physical-production authority remains in Spec 106.

## Security and correctness audit

- Controller, User, and Provider identities are distinct and bound read-only at
  runtime; images, profiles, logs, and evidence reject secrets/private keys.
- Permission encryption, NAC-ABE routing, UserToken/ProviderToken, replay,
  provider permission, lease/attempt/deadline/cancellation, artifact digests,
  and exactly-one terminal response remain mandatory.
- GPU PASS requires allocated GRES-to-node-to-UUID-to-container-to-provider/stage
  backend correlation; visibility-only and undeclared CPU fallback fail.
- Single-node evidence must have one node/NFD, three distinct Provider PIDs/GPUs,
  and no claimed cross-node edge. Multi-node evidence must have at least two
  nodes/NFDs and at least one dependency crossing an observed NDN face.
- A selected-transport failure blocks multi-node eligibility. Failure of an
  unselected diagnostic transport cannot invalidate a working selected transport.

## Validation and evidence audit

Evidence authority remains ordered and non-substitutable:

```text
implemented source
  != offline fixture pass
  != SIF/runtime substrate pass
  != single-node NDNSF-DI candidate execution
  != multi-node placement extension
  != seven-size/performance matrix
  != physical production authority
```

A pre-start blocker leaves the live task open. A failure after
`CANDIDATE_EXECUTION_STARTED` is retained as a negative result with a precise
failure boundary. Promotion must be atomic and checksum-valid before PASS/FAIL
authority is accepted. No negative, slow, or ambiguous submission may be
silently rerun under the same identity.

## Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Real NDNSF-DI is primary; standalone is oracle/local reference only. |
| Experimental design | Yes for implementation | Placement is staged and both overhead contrasts are matched. |
| Architecture/ownership | Yes for implementation | Process-map and per-node NFD ownership are explicit. |
| Security/correctness | Yes for implementation | Required positive and mutation paths are explicit. |
| Task executability | Yes | 147 unique sequential tasks with size-local/per-cell closure. |
| Validation/evidence | Yes for implementation | Placement-conditional schema and mechanical execution boundary exist. |
| Migration/rollback | Yes | Candidate parser migration and immutable release rollback are tasked/contracted. |
| Code capability | No, expected | Generation wiring, topology supervisor, GPU release, and journal remain open tasks. |
| Live iTiger execution | No | Zero Spec 110 jobs were submitted during audit/remediation. |

## Metrics

- User stories: 6
- Functional requirements: 36
- Success criteria: 13
- Tasks: 147, all initially open
- Requirement coverage: 36/36
- Duplicate task IDs: 0
- Placeholders requiring design resolution: 0
- Unresolved Critical / High / Medium / Low document findings: 0 / 0 / 0 / 0
- Controlled implementation/external risks: generation wiring, topology adapter,
  GPU OCI/SIF, live cluster facts, and 32B/72B capacity

## Deterministic checks

The final audit rerun records:

```text
audit_speckit_structure.py --strict: PASS
  functional_requirements=36, success_criteria=13, user_stories=6
  tasks=147, traced_requirements=36, blockers=0, warnings=0
check-prerequisites.sh --require-tasks --include-tasks: PASS
JSON Schema Draft 2020-12 self-check: PASS
  valid single-node and multi-node examples accepted
  single-node cross-edge, multi-node missing-edge, CPU backend,
  SUBMISSION_UNKNOWN executed PASS, and empty process records rejected
Task IDs: 147 unique and sequential T001-T147
git diff --check: PASS
GSD validate health: healthy; one unrelated in-progress phase summary warning
CodeGraph status: up to date; 7,295 files, 160,816 nodes
```

## Evidence limitations

- Current iTiger login, quota, GRES, node addresses, and inter-node firewall were
  not queried during this document-only audit.
- No OCI/SIF was built, transferred, or executed.
- No Spec 110 Slurm job was submitted.
- No Qwen model was inferred, exported, or measured under Spec 110.
- 32B/72B storage and placement remain capacity risks until live admission.

## Next actions

1. Execute T001-T030 offline and retain the foundation gate.
2. Build/publish/materialize the OCI/SIF and complete T031-T049.
3. Complete generation-session/backend work T065-T073, then execute the first
   one-node/three-GPU 0.5B cells T074-T084.
4. In parallel after T049, qualify the selected cross-node transport T050-T064;
   only T064 PASS plus T084 PASS unlocks T088-T089.
5. Continue the seven-size single-node ladder T090-T105 before performance.

## Post-Spec-111 amendment checklist

- [x] Docker/OCI is build/distribution only; iTiger compute uses exact SIF with Slurm/Apptainer
- [x] Current/pre-Spec-111 release is substrate evidence only and cannot prove the new APP workflow
- [x] Infrastructure allocation, APPDeployment and request handles/states are distinct
- [x] Runtime handoff binds revision, OCI/SIF, model, process-map/network, identity/state and authorization digests
- [x] Models/artifacts/identities are read-only; RuntimeJournal state is identity-partitioned persistent read-write
- [x] Generic process-map v2 derives Provider roles from the revision and preserves frozen v1 evidence
- [x] Every project process runs inside the same exact SIF with shared node-run and per-Provider GPU mapping
- [x] Single-node post-Spec-111 PASS precedes selected-transport multi-node use
- [x] No live job is authorized by this document revision; T219-T232 remain future work
