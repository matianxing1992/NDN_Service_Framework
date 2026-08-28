# Specification Quality Checklist: Predictive API Replacement

## Intent and Boundary

- [X] `start()` / `push()` / `flush()` is the sole target high-level provider API.
- [X] Old high-level `start(initial...)` / `announce()` / `publish()` is explicitly removed.
- [X] `start_predictive()` compatibility naming is explicitly removed.
- [X] Low-level generic Core primitives are distinguished from the removed facade.
- [X] Spec 147's facade is superseded while frozen evidence remains immutable.
- [X] Rollback uses a pinned old binary, not a retained compatibility path.

## Correctness and Security

- [X] Exact App-signed source wire ownership is explicit.
- [X] Validator invocation and fail-closed admission are required.
- [X] Unequal-length repair metadata, concurrency, stop, late join, and equivocation are specified.
- [X] C++ and Python replacement surfaces are symmetric.
- [X] Core workload/UAV/codec branches are forbidden.

## Evidence Integrity

- [X] Earlier additive interpretation and false closure are withdrawn.
- [X] Existing focused-test failure is retained as a blocker.
- [X] Import/syntax/declaration checks are not accepted as behavior evidence.
- [X] Fresh two-node MiniNDN evidence is required.
- [X] Old/new claims require immutable binary hashes and matched configuration.
- [X] Real controller/Drone/Ground Station processes and node placement are explicit.
- [X] Preflight, functional smoke, and formal measurement are distinguished.
- [X] The new binary cannot retain a Mapping-first selector for comparison.
- [X] Endpoint runtime proof, formal cells, metrics, and artifacts are frozen.
- [X] Specification, plan, contract, tasks, traceability, audit, and closure status agree.

Implementation completion is tracked only in `tasks.md`; it is not a
specification-quality checklist item.
