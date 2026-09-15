# B187-LOCAL-CLOSURE Evidence

**Task**: T001 Candidate closure and pair mutation gate  
**Batch**: B187-LOCAL-CLOSURE  
**Status**: PARTIAL  
**Base**: `6bade78d`

## Scope and five lanes

- **production callers**: `build-sif-app.py:publish()` keeps its existing CLI
  contract; no caller or entrypoint was duplicated.
- **implementation**: immutable base, candidate, build record and Apptainer
  identity/version/digest are checked before opening the publication parent,
  locks, marker or staging recovery. A changed base record therefore has no
  publication-side effect.
- **tests/fixtures**: `test_publish_rejects_changed_base_before_publication_side_effects`
  uses a changed-base mutation, replaces the lock operation with a failure
  sentinel, and verifies that stale staging, output and manifest are unchanged.
- **build/migration wiring**: existing TigerCluster paths and the
  `build-sif-app.py` entrypoint remain unchanged; no new SIF workflow was added.
- **evidence/operations**: static review and focused offline checks are
  recorded here. No regular base SIF or host-gate manifest is available on this
  host, so no SIF build, upload, MiniNDN or TigerCluster operation was started.

## Verification

Commands run from the repository root:

```text
python3 -m py_compile Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-sif-app.py Experiments/TigerCluster/tests/test_sif_app.py
pytest -q Experiments/TigerCluster/tests/test_sif_app.py -k 'publish_rejects_changed_base or delivery_scripts_have_isolated_runtime_contract or build_driver_rejects_overwrite' --disable-warnings --maxfail=1
```

Result: **3 passed**.

The frozen review snapshot is
`.codex-tmp/spec187-t001-review-20260915-r1/`; its exact diff is
`diff.patch` with SHA256
`3988566523499e71d55c176a8068788d99f6fc5cf972549c863021553b61a4bd`.
The read-only official `review-agent` gate returned `STATIC_PASS` and no P0–P3
findings. It explicitly confirmed that changed-base validation precedes locks,
marker handling and staging recovery. The review did not build or run a SIF.

## Remaining boundary

The closure mutation gate is implemented and statically reviewed, but T001 is
not a complete pair acceptance: a regular base SIF and matching host-gate
manifest are still required to exercise the existing handoff/render/build and
materialization path. Keep the task `PARTIAL` until that external input exists.
