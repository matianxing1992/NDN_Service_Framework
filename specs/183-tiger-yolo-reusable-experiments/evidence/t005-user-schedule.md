# T005 partial: maintained one-shot User execution

Date: 2026-09-07. Branch: TigerClusterExperiments.

## Source-derived corrections

- The maintained `yolo_2x2/user.py::_load_yolo_ack_driven` rejects request IDs
  that are not absolute NDN names. The preview's bare SHA-derived identifier
  was therefore unusable. `resolve_run_plan` now emits namespace-scoped NDN
  request names, and the process tests reject bare/duplicate identities.
- `load_or_generate_deployment` unconditionally writes its generated policy.
  User-generated policy must go to writable per-invocation `/output`, not the
  immutable `/config` input mount. The launcher explicitly supplies this path.
- ACK-driven `main` returns after one request, before the legacy sequential
  options. `apps/yolo.py::run_requests` invokes the maintained entrypoint once
  per schedule row rather than pretending `--sequential-requests` repeats it.
- Request output locations in the run plan now agree with the worker's
  `node0/user/requests/<index>` mapping. No silent path rewriting on Tiger.

## Implementation and boundaries

The new application component consumes a validated normal run schedule and
launches one warmup plus three measured Users for two-node-gpu, or one plus
one for local-cpu/single-node-gpu. Each receives a distinct request ID and
exclusive output directory; Provider services stay alive. User HOME/state
persist within the run and the envelope key stays in that HOME. Only the User
receives the canonical package mount; Provider model transport remains NDN.

The shared finite-process supervisor now accepts a live service/peer check and
an explicit process owner. NodeRuntime retains finite-process cleanup records
and HOME leases until the process group is gone. A timeout/nonzero exit retains
its original failure; forced/uncertain cleanup cannot continue the schedule.
Existing output directories and duplicate invocations are not overwritten.

The caller must supply `accept_request(request, output)` and that evidence
validator must raise on invalid results. There is no default success validator.
T006 must implement this owner; a process exit is not the numerical/graph
verdict. Negative-dependency is explicitly rejected by the normal scheduler
until its real post-Selection fault owner and collector are wired.

## Focused evidence only

`python3 -m pytest -q Experiments/TigerCluster/tests --tb=short --junitxml=Experiments/TigerCluster/results/spec183-user-schedule-r2/junit.xml`

Final regression: **287 passed in 19.64s**, exit 0, including finite-kind
metadata preservation. Earlier r1 (287/17.43s) remains separately retained.
Coverage includes actual finite subprocesses, live persistent service checks,
per-request call count/argv/output uniqueness, stop-on-first process/evidence/
peer failure, no same-ID reuse, bad schedule/input rejection and cleanup.
External Apptainer/native model execution is substituted at the OS boundary;
synthetic configuration and validator fixtures are explicitly not qualification.

## Remaining

T005 is still partial: canonical signed-material preparation, Controller/Repo
launch and secure readiness, real model/dependency/negative integration remain.
T006 collector, final T004/T002 wiring, T007 audit and T008–T017 are pending.
No SIF build, upload, GPU job or real inference was executed in this checkpoint.
GSD health retains its known W019 for the noncanonical handoff; repository
tasks/evidence, not the old Spec168 STATE.md, remain progress authority.
