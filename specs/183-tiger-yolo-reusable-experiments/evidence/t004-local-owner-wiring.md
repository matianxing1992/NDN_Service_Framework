# Local owner and frozen collector wiring — 2026-09-07

Status: source IMPLEMENTED, component evidence accepted; NOT native/SIF/GPU
qualification. T004 and T007 remain open because remote staging/run and actual
runtime evidence are incomplete. No model or cluster campaign was started.

## Closed source gaps

- Local/collect no longer wait for the read-only content checker to return
  READY: its documented result is always NOT_EVALUATED. They require VERIFIED
  content integrity. Local then consumes the existing host gate validator,
  binding its source seal to the runtime plane's native manifest and requiring
  all nine native artifacts with matching build/final hashes. Generic PASS
  markers, missing host receipts, wrong source and missing extensions reject.
- Local validates the prepared plan/candidate and re-enters the verified frozen
  CLI. execute_local_run binds the profile's package, oracle, input and budgets;
  calls the existing provision/rank owners; validates each request before the
  next; publishes collection input only after complete request coverage and
  cleanup; then delegates to collect. Failure outputs remain exclusive.
- The installed prepare owner retains graph-port and catalogue-body digests
  directly from the authenticated YOLO adapter. Raw ONNX hash, planning graph
  identity, runtime identity and per-request MODELROOT remain distinct.
- The explicit harness snapshots the existing generic NumPy reference/decoder
  modules into owners/yolo_reference.py and owners/yolo_tensor_bundle.py. They
  are not separately maintained Tiger code.
  Its loader avoids importing native-dependent DI package parents. Existing
  Tiger/lib compatibility symlink is resolved by an explicit canonical-owner
  mapping at build time; frozen bundles reject symlinks and missing files.
- Dispatch rejects a profile changed between the content check and resolved
  profile read. Collection re-enters the frozen CLI and rechecks retained data.

## Evidence

`Experiments/TigerCluster/results/spec183-local-owner-wiring-20260907/focused.xml`:
113 passed, one fixture failure, 23.64s. Selection:

```bash
python3 -m pytest -q --tb=short \
  Experiments/TigerCluster/tests/test_yolo_local_execution.py \
  Experiments/TigerCluster/tests/test_yolo_bundle.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py \
  Experiments/TigerCluster/tests/test_yolo_operator.py \
  Experiments/TigerCluster/tests/test_yolo_prepare_entrypoint.py \
  Experiments/TigerCluster/tests/test_yolo_provision.py \
  tests/python/test_spec183_numerical_reanalysis.py \
  --junitxml=Experiments/TigerCluster/results/spec183-local-owner-wiring-20260907/focused.xml
```

The prepare fixture previously returned None instead of an adapter. It now
returns explicitly declared graph/catalogue identities and checks they reach
the receipt. Only that module was rerun: **12 passed in 0.33s**, retained as
`preparation.xml` in the same directory. Merging the latest record per test
identity gives **114 unique component cases with passing evidence**, not a
claim that the initial invocation was wholly green. Earlier source-freezing
failure exposed Tiger/lib's compatibility symlink and led to the exact owner
mapping. A final import audit also found the tensor decoder's host DI import;
it now uses the frozen generic decoder alongside the reference module. Those
findings are documented in the failure log.

After adding the decoder to the closure, only the affected bundle/local-owner/
numerical modules were checked again: **44 passed in 12.41s**, `frozen-owners.xml`.
A fresh isolated interpreter additionally forbids every ndnsf/py_repoclient
import while loading both frozen NumPy owners: **1 passed in 2.28s**,
`isolated-owner.xml`. These are refinements of the same 114 unique cases, not
extra model runs or native evidence. The explicit harness now has 24 files.

The updated real profile passed `submit.py check --stage dispatch` integrity
validation (exit 78, qualification NOT_EVALUATED). The formatted command output
is retained as `profile-check.json` alongside the JUnit files. I/R identities
remain `36c909db` / `eeb8afa0`; E is `e742ca63`, harness `a516ba36`, profile
document `a0ab4798`. Rendering refreshed only changed profile references and
frozen source bytes; it did not build a SIF or run inference.

The local composition tests deliberately replace provision/rank boundaries;
the real NumPy owner is frozen/loaded and numerical reanalysis is checked
separately. They prove connection/order/rejection, not a running native User
or YOLO model. Another client's host build was confirmed live by PID 1206581
and was not edited, restarted or claimed as qualification.

Next: remote typed prerequisites/staging/Slurm owner, then T007 re-audit. The
registered host → MiniNDN → exact-SIF → bounded Tiger gates remain required;
old source seals and old harness inventories cannot qualify the changed code.
