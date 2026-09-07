# Spec183 design-to-code convergence audit

**Date:** 2026-09-07
**Branch:** `TigerClusterExperiments`
**Verdict:** **BLOCKED / NOT READY FOR FORMAL VALIDATION**

This is the required production-wiring audit for T007. It is deliberately a
blocking audit, not a completion claim. The audit used the current Spec183
documents, `git status`, the CodeGraph index, exact source inspection, and the
registered focused test suite. No SIF, model, MiniNDN, or Tiger job was
started.

## Verified component boundaries

| Boundary | Current evidence | Verdict |
| --- | --- | --- |
| `applicationName + '/sync'` | `runtime.yolo_profile.application_sync_prefix()` is consumed by projection, NFD route setup, and startup validation; malformed names and legacy `/group` are rejected. | PASS (component only) |
| Worker lifecycle | `NodeRuntime`, owned role homes, process groups, finite User calls, GPU/PID probes, and cleanup readers have source-shaped and real short-lived process tests. | PASS (component only) |
| Preparation/public material | `apps/yolo.py::prepare_in_container` and preparation inventory enforce isolated mounts, public recipient maps, protected epoch, and no Provider model mount. | PASS (component only) |
| Normal request schedule | `run_requests` invokes the maintained ACK-driven User once per warmup/measured request and keeps Providers alive. | PASS (argv/process-boundary tests only) |
| Result boundaries | lifecycle, numerical, dependency, device, cleanup, node-receipt, and expected-rejection validators reject the registered mutation classes. | PASS (retained/component evidence only) |
| SIF dispatch preflight | Spec183 host receipt and exact-SIF preflight fail closed before Apptainer/build calls and inspect imports/entrypoints/`ldd` when a real SIF is supplied. | PASS (negative/fixture boundary only) |

The merged registered focused selector currently reports **885 passed**. This
number is not a runtime qualification result; the tests use doubles or
source-shaped receipts where the physical inputs are unavailable.

## Blocking production findings

| ID | Requirement | Finding | Owner / earliest gate |
| --- | --- | --- | --- |
| T007-B1 | FR-002, FR-018 | `Experiments/TigerCluster/profiles/yolo-two-node.json` is absent. No real partition/account/GPU/memory/SIF/model/oracle references can be checked. | T001 external inputs, then T004 |
| T007-B2 | FR-012, FR-018 | `Experiments/TigerCluster/jobs/yolo/run.sbatch` is absent and `jobs/yolo/submit.py` exposes only read-only `check`. There is no production `prepare/local/submit/collect` command path or Slurm query/recovery boundary. | T004 |
| T007-B3 | FR-007, FR-008, FR-009 | `run_normal_node` is a coordinator library, not connected to a real operator allocation. No real NFD, Controller, Repo, Provider, User, or cross-node signed-data run has occurred. | T005/T007, then T009/T010 |
| T007-B4 | FR-005, FR-006 | Locked source archives/build inputs, the signed YOLO package/registry/oracle, and the local base SIF are not all present. The exact-SIF preflight therefore cannot produce a candidate. | T002/T008/T011; `WAITING_EXTERNAL_INPUT` |
| T007-B5 | FR-010, FR-011 | The final collector is implemented and exercised with retained fixtures, but no real native response, optimized graph, CUDA execution, or independent model oracle has reached it. | T006, then T008–T011 |
| T007-B6 | FR-014 | The required unit → integration → MiniNDN → exact-SIF → Tiger sequence cannot start until T007 closes; current component tests do not satisfy that ordering. | T007 gate |

## Effective-field audit

The current library path consumes the fixed ACK timeout, request deadline,
protected epoch, candidate identifiers, application Sync prefix, role
identity, provider/service names, output paths, and cleanup budgets in its
component-level argv checks. The following profile-owned fields are not yet
consumed by a production command because the profile/launcher is absent:

* Apptainer executable/version and exact SIF path/hash;
* Slurm partition, account, constraint, node/GPU/memory/walltime allocation;
* immutable harness/model/oracle references and their candidate E binding;
* shared run/lock roots and allocate-once `SubmissionJournal` transitions;
* `prepare`, `local`, `submit`, and `collect` argv/env plus job reconciliation.

Consequently, no field-consumption or no-leftovers claim is allowed yet.

## Required closure before changing this verdict

1. Receive and independently hash the locked source/base/package/model/oracle
   inputs; keep any missing item explicitly `WAITING_EXTERNAL_INPUT`.
2. Implement the real profile and `run.sbatch` around the existing helpers;
   do not add a placeholder file merely to satisfy the harness inventory.
3. Wire the five operator commands and allocate-once journal, including
   `SUBMISSION_UNKNOWN` reconciliation without blind resubmission.
4. Re-run T002–T006 through the actual command boundaries, then rerun this
   audit with CodeGraph and exact argv/env evidence.
5. Only after a PASS audit run the mandated local unit/integration/MiniNDN and
   local-SIF gates; no Tiger submission is authorized before those receipts.

## Evidence references

* [input inventory](input-inventory.md) — locked revisions, missing physical
  inputs, and external-input boundary.
* [T005 startup coordination](t005-startup-coordination.md) — component
  readiness and canonical Sync prefix, explicitly not native/Tiger evidence.
* [T002 integrity](t002-integrity.md) — staged closure and fail-closed
  preflight boundaries.
* [experiment profile contract](../contracts/experiment-profile.md) — the
  five-command interface and qualification gates.
