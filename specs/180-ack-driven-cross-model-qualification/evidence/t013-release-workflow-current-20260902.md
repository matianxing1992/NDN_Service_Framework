# Spec180 T013 Release-Workflow Implementation Checkpoint

**Status**: partial implementation; no remote side effect

## Implemented

- `packaging/ndnsf-di-container/jobs/spec180/profile.json` records one fixed
  Apptainer 1.5.3, `/bundle`, host-NFD, ONNX Runtime profile for the YOLO and
  Qwen gates.
- `scripts/spec180_release.py` performs duplicate-field rejection, strict
  profile/run-record allowlisting, fixed resource/timing validation, digest
  checks, deterministic Slurm argv/environment rendering with explicit
  `--export=NONE,SPEC180_*` assignments, and scheduler invocation only after
  rendering succeeds.
- `scripts/validate_spec180_results.py` requires candidate digest agreement,
  two completed requests, clean child exits, protocol/result/runtime oracles,
  redaction, and cleanup before accepting a terminal result.
- The public `submit.sh` wrapper rejects unknown gates, missing inputs, and
  direct argument expansion before delegating to the renderer.
- `run-functional.sh` validates the fixed Apptainer 1.5.3 binary and SIF
  digest, probes the case runner inside the image, and enters `/bundle` with
  `--nv --cleanenv`; it never runs a host-side runner or relies on the Slurm
  submit-directory cwd.
- `scripts/spec180_inventory.py` builds a deterministic source-bound inventory
  record, parses the native selector tree and Spec180 pytest node IDs, and
  refuses to write an inventory when a registered Y-A/Y-B/Y-N/Q-C/Q-W
  entrypoint is missing or an identity/command digest is inconsistent.
- `scripts/run_spec180_local_gate.py` provides the isolated execution slice:
  it validates every entry digest before launch, snapshots `inventory.json`
  into a fresh evidence root, runs one supervised child per entry, records PID,
  oracle, exit/signal/timeout/cleanup and redaction fields, and refuses stale
  non-empty output roots. It binds each MiniNDN case ID to its registered
  argument tuple and rejects command-shape/path substitutions.

## Focused evidence

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec180_release_workflow.py
6 passed

python3 -m pytest -q tests/python/test_spec180_inventory.py
5 passed

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec180_local_gate.py
9 passed
```

The tests prove profile/run-record mutations fail before an injected scheduler
adapter is called, and that a valid record renders the registered 1500 ms ACK,
60000 ms request, and 5000 ms SVS timings, and that export-delimiter mutations
are rejected before scheduling. The inventory tests additionally prove nested
Boost selector parsing, pytest node binding, candidate digest binding, and
fail-closed missing-case handling. The local-gate tests prove source-digest
preflight, one-child supervision, case-oracle enforcement, redaction handling,
inventory snapshotting, command-shape enforcement, and stale-output rejection.
They do not prove a
scheduler, real MiniNDN case, SIF, CUDA, or Tiger execution.

## Remaining T013 work

The candidate-bound inventory materialization, real MiniNDN case runner,
inside-SIF replay, and terminal result integration are still owned by T011,
T013, and T015--T020. `run-functional.sh` fails closed until the T015 case
runner exists; this is intentional and is not qualification evidence.
