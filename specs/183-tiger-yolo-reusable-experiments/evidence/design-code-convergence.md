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

The direct full `Experiments/TigerCluster/tests` run currently reports
**806 passed in 34.12s**. A prior broader registered selector reported 885
tests; that historical count is not reused as current evidence. Neither count
is a runtime qualification result: the tests use doubles or source-shaped
receipts where the physical inputs are unavailable.

## Blocking production findings

| ID | Requirement | Finding | Owner / earliest gate |
| --- | --- | --- | --- |
| T007-B1 | FR-002, FR-018 | `Experiments/TigerCluster/profiles/yolo-two-node.json` is absent. No real partition/account/GPU/memory/SIF/model/oracle references can be checked. | T001 external inputs, then T004 |
| T007-B2 | FR-012, FR-018 | `run.sbatch` now exists and `jobs/yolo/submit.py` exposes `check`, `prepare`, `local`, `submit`, and `collect` plus a hidden allocation-bound `run`. The commands remain fail-closed: no qualified profile/receipt has reached a real Slurm query, worker launch, recovery boundary, or collector result. | T004/T007 |
| T007-B3 | FR-007, FR-008, FR-009 | `run_normal_node` and `runtime.yolo_operator.finalize_normal_collection` now provide the production-shaped rank and worker-to-collector seams. The finalizer refuses partial rank returns and the handoff writer re-reads every retained node receipt before publishing `collection-input.json`; no real NFD, Controller, Repo, Provider, User, or cross-node signed-data run has occurred. | T005/T007, then T009/T010 |
| T007-B4 | FR-005, FR-006 | Locked source archives/build inputs, the signed YOLO package/registry/oracle, and the local base SIF are not all present. The exact-SIF preflight therefore cannot produce a candidate. The bounded compute probe shows Apptainer 1.5.3 while the login host has 1.3.4; the matching local `/opt/apptainer/1.5.3/bin/apptainer` is now available, but must be used explicitly rather than the login/default binary. | T002/T008/T011; `WAITING_EXTERNAL_INPUT` |
| T007-B5 | FR-010, FR-011 | The final collector is implemented and exercised with retained fixtures, but no real native response, optimized graph, CUDA execution, or independent model oracle has reached it. | T006, then T008–T011 |
| T007-B6 | FR-014 | The required unit → integration → MiniNDN → exact-SIF → Tiger sequence cannot start until T007 closes; current component tests do not satisfy that ordering. | T007 gate |

## Effective-field audit

The rank-operator seam and application coordinator consume the fixed ACK
timeout, request deadline, protected epoch, candidate identifiers, exact
`applicationName + '/sync'` prefix, role identity, provider/service names,
output paths, endpoints, and cleanup budgets in component-level argv and
lifecycle checks. The following profile-owned fields are still not consumed
by an actual production dispatch because no qualified profile has been
accepted and the launcher is intentionally fail-closed:

* Apptainer executable/version and exact SIF path/hash;
* Slurm partition, account, constraint, node/GPU/memory/walltime allocation;
* immutable harness/model/oracle references and their candidate E binding;
* shared run/lock roots and allocate-once `SubmissionJournal` transitions;
* `prepare`, `local`, `submit`, and `collect` argv/env plus job reconciliation
  against a real Slurm allocation and collector receipt.

Consequently, no field-consumption or no-leftovers claim is allowed yet.

## Required closure before changing this verdict

1. Receive and independently hash the locked source/base/package/model/oracle
   inputs; keep any missing item explicitly `WAITING_EXTERNAL_INPUT`.
2. Provide the real profile and complete the existing `run.sbatch`/operator
   wiring around the existing helpers; do not add a placeholder file merely
   to satisfy the harness inventory.
3. Connect the five operator commands and allocate-once journal, including
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
* [T006 collector handoff](t006-collector-handoff.md) — rank-join and immutable
  worker-to-collector input boundary, still fixture/component evidence only.
* [T002 integrity](t002-integrity.md) — staged closure and fail-closed
  preflight boundaries.
* [experiment profile contract](../contracts/experiment-profile.md) — the
  five-command interface and qualification gates.
