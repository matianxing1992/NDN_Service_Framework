# Pre-Implementation Audit: NDNSF-DI OCI Deployment Adapters

**Mode**: pre-implementation
**Date**: 2026-07-12
**Audited artifacts**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`, `tasks.md`, `traceability.md`
**Code baseline**: current working tree, CodeGraph index current at 7,196 files / 159,767 nodes

## Verdict

`PASS` for implementation readiness. The design answers the requested OCI abstraction and iTiger adapter without moving scheduler, inference, security, or physical-production authority into packaging. All initially observed low-severity contract/task defects were repaired before the final audit. This verdict does not claim that any of the 160 implementation or live-acceptance tasks is complete.

## Findings

| ID | Severity | Dimension | Location | Finding | Resolution |
|---|---|---|---|---|---|
| A-001 | Low | Evidence integrity | `contracts/container-evidence.schema.json` | Initial SIF conditional allowed an arbitrary materialization ID even though the requirement calls for SHA-256. | Repaired: SIF `materialization.id` now references the SHA-256 definition. |
| A-002 | Low | Validation design | `tasks.md` T134 | Initial two-node iTiger network probe was bounded but did not specify a concrete wall-time ceiling. | Repaired: task now requires exactly one five-minute CPU probe. |
| A-003 | Low | Task executability | `tasks.md` T160 | Initial completion-summary task lacked an exact output path. | Repaired: output is `completion-summary.md` in the feature directory. |

No Critical, High, or Medium findings remain.

## Code-reality checks

- Current `OnnxRuntimeModelRunner` selects `CUDAExecutionProvider` when available, throws when CUDA is required and CPU fallback is disabled, and emits selected-provider/device evidence. The packaging design consumes this truth rather than reimplementing provider selection.
- Current runtime and release-gate code preserve `physicalProductionOverall=DEFERRED`; tests explicitly reject attempts to promote it to PASS. The Spec 108 evidence schema keeps the same boundary and names Spec 106 as owner.
- Existing `packaging/ndnsf-di-systemd/` contains create/install/rollback/uninstall and staging validation surfaces. Spec 108 preserves it and adds baseline/rollback tasks.
- No first-party `packaging/ndnsf-di-container/` implementation currently exists. Therefore every implementation task remains unchecked and no planned path is falsely represented as built.
- CodeGraph was current during audit; text search was used only for exact evidence fields, test assertions, scripts, and packaging filenames.

## Traceability gaps

None at the design level:

- 36/36 functional requirements appear in `traceability.md`;
- 12/12 success criteria appear in `traceability.md`;
- all six user stories have an independent test and task phase;
- tasks T001-T160 are unique and sequential;
- no unresolved clarification marker or old Docker-only package root remains.

Implementation evidence is intentionally absent and must be produced by the unchecked tasks.

## Readiness scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | OCI source plus two explicit adapters matches the request. |
| Architecture and ownership | Yes | Shared release/evidence core; adapters own environment lifecycle only. |
| Security/correctness | Yes | External identities, read-only binds, secret scans, fail-closed GPU, exact-job cancellation. |
| Task executability | Yes | 160 ordered tasks with exact paths, tests, checkpoints, and bounded live jobs. |
| Validation/evidence | Yes | Offline, local, MiniNDN, cloud, and bounded iTiger layers are distinct. |
| Migration/rollback | Yes | Digest rollback and unchanged systemd fallback are explicit. |
| Code reality | Yes | Current backend and DEFERRED behavior were verified; new package is not claimed to exist. |

## Metrics

- User stories: 6
- Functional requirements: 36
- Success criteria: 12
- Tasks: 160 (0 complete at audit time)
- Parallel-marked tasks: 40
- Requirement traceability: 36/36
- Success-criterion traceability: 12/12
- Unmapped tasks: 0 identified
- Placeholders: 0
- Remaining Critical / High / Medium / Low findings: 0 / 0 / 0 / 0

## Assumptions and evidence limits

- iTiger partition, GRES labels, versions, quotas, and node layout are external mutable facts and must be rediscovered by preflight.
- Job 145855 proves only preliminary scheduler/Apptainer/GPU/scratch capability; it did not execute the NDNSF-DI candidate.
- Compute-node multi-node TCP/UDP 6363 and NFD addressability have not been measured; support remains disabled pending T134.
- Final OCI registry/authentication/signing policy and the final GPU compatibility matrix remain implementation inputs.
- No Docker Compose, candidate-bound iTiger inference, RTX 6000, H100, or physical deployment acceptance was run during this document-only revision.
- The dirty working tree contains pre-existing Spec 107/runtime changes; this revision does not modify them.

## Validation commands completed for this revision

```text
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
  -> PASS: required Spec Kit artifacts resolved

.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
  -> PASS: research, data model, contracts, quickstart, and tasks present

jq empty contracts/*.json
jsonschema.Draft202012Validator.check_schema(...)
  -> PASS: both JSON schemas are syntactically valid Draft 2020-12 schemas

codegraph status .
  -> PASS: index up to date

packaging/ndnsf-di-systemd/validate-staging.sh \
  --work-root UNIQUE_TMP \
  --candidate-id spec107-c1-111111111111-222222222222-333333333333-444444444444-555555555555-666666666666 \
  --plan-digest sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
  -> PASS: release N/N+1, rollback, uninstall, Repo preservation, nine static hardening directives
  -> note: systemd-analyze emitted expected sandbox varlink warnings but returned success

python3 -m py_compile \
  packaging/ndnsf-di-container/lib/*.py \
  packaging/ndnsf-di-container/lib/adapters/*.py \
  packaging/ndnsf-di-container/bin/ndnsf-di-deploy \
  tests/container/contract/*.py tests/container/unit/*.py
tests/container/run.sh
  -> PASS: 60 tests; profile, evidence, release, CLI, adapter, redaction,
     materialization, Schema-copy, atomic-promotion, OCI rootfs/build inputs,
     entrypoint/health, Compose rendering/lifecycle, and mount contracts

Spec 108 offline Slurm/Apptainer matrix
  -> PASS: resource/GRES rendering, injection rejection, scheduler parsing,
     storage/quota separation, OCI-to-SIF identity, exit preservation,
     GPU UUID mapping, exactly-once submission, bounded wait/cancel, and scratch policy
  -> live Slurm/GPU submissions: 0

docker compose -f packaging/ndnsf-di-container/adapters/docker-compose/compose.yaml config --quiet
  -> PASS: one host-scoped NFD plus controller/provider; only NFD publishes ports

docker info
  -> BLOCKED: permission denied for /var/run/docker.sock in the managed sandbox
sudo -n docker info
  -> BLOCKED: sandboxed sudo has no setuid root authority
  -> T052 remains unchecked; no image build, Compose readiness, or recreate PASS claimed
```

## Next actions

1. Begin Phase 1 and Phase 2; do not submit another GPU job before offline contracts and the final OCI release exist.
2. Complete the CPU OCI/Compose vertical slice (US1) and the offline Slurm adapter tests in parallel after the common contract is stable.
3. Use the five-minute RTX 5000 job only after digest/SIF/evidence finalization paths pass offline tests.
4. Preserve the measured outcome of every live job without automatic rerun.
5. Hand physical performance, production security, real-network/UAV, and soak closure to Spec 106.
