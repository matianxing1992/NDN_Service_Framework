# Spec180 Local-Suite Inventory Contract v1

The local qualification gate is a source-bound inventory, not an informal
choice of commands. T013 owns the inventory schema and renderer; T015
materializes the exact entries for one candidate and executes them. The local
YOLO entries invoke `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` directly;
the remote SIF jobs invoke the T011-owned thin
`scripts/run_spec180_case.py` dispatcher, which must resolve to the same
candidate-bound implementation. These are two launch boundaries, not two
protocol implementations.

## Inventory entry

Each entry is a canonical record with:

- `id`, `kind` (`cpp-suite`, `cpp-selector`, `python-selector`, or
  `minindn-case`);
- repository-relative executable/selector/entrypoint and its command digest;
- source revision, candidate identity, and effective-configuration digest;
- required backend identity (`cpu-onnxruntime` for local cases);
- timeout and cleanup policy; and
- an evidence path for the supervised child record.

The inventory MUST include the unit and integration selectors mapped by the
T014 owner audit to the production YOLO/coordinator/placement/assembly/security
path, all active `tests/python/test_spec180_*.py` YOLO/release selectors, and
the three formal MiniNDN entrypoints `Y-A`, `Y-B`, and `Y-N`. It MUST NOT add
Q-C, Q-W, QWEN-F, or unrelated whole-repository suites as Spec180 acceptance
dependencies. A missing, duplicate, unregistered, or ambient entry fails the
local gate before execution.

## Execution record

Every inventory entry runs exactly once in its own supervised child. The three
formal MiniNDN cases are entries in this same inventory, not a second execution
phase. The record captures
the exact command, PID, start/end timestamps, exit status, signal, timeout flag,
stdout/stderr redaction result, protocol/result oracle references, and cleanup
status.

The inventory and child records are candidate-bound. Changing a selector,
binary, harness, effective configuration, or redaction rule invalidates local
qualification and returns to the earliest controlling gate in the Spec180
invalidation matrix.

The supervised runner writes an immutable `inventory.json` snapshot into a
fresh evidence root before starting the first child. An existing non-empty
root is rejected to prevent stale records from being mixed with a new
candidate. The aggregate record carries both the canonical inventory digest
and the snapshot-file digest. Each child record includes its PID and an
explicit oracle: `exit-code-zero` for native/Python selectors, or the exact
`SPEC180_CASE_RESULT status=PASS case=<id>` marker for a YOLO MiniNDN case. A
zero exit without the registered YOLO case marker is `CASE_ORACLE_MISSING`, not
a pass.
The runner binds each case ID to its registered Y-A/Y-B/Y-N argument tuple;
changing a case's command arguments is a command-shape failure,
not a new workload.
Redaction covers both secret-like field names and their assignment values;
the stored log must never contain the original value.
