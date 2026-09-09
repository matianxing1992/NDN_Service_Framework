# T010 host-gate producer follow-up

The shared validator and `Experiments/TigerCluster/tools/spec183_host_gate.py`
were exercised together with the boundary suite:

```text
python3 -m pytest -q \
  tests/python/test_spec183_yolo_host_gate.py \
  tests/python/test_spec183_yolo_build_dispatch.py \
  tests/python/test_spec183_host_gate_producer.py
18 passed
```

The tests cover source/base/application binding, four-role execution evidence,
Merge's native postprocess semantics, numerical and lifecycle binding, clean
network/process teardown, hash-valid empty evidence, and wrong dependency
boundaries.  The producer validates negative boundaries before creating its
combined normal log, so a rejected dependency input leaves no partial receipt
to reuse.

An attempted join using the real v48 normal output, a v49 Y-N-E protected-grant
rejection, and v49 Y-N-C as the dependency input failed closed with
`HOST_GATE_FAILURE_BOUNDARY`.  This is the expected result: Y-N-C reports
`PLACEMENT_DECISION/NO_FEASIBLE_CANDIDATE`, not the required
post-Selection `DEPENDENCY_DATA_MISSING` or `PEER_FAILURE`.  No host gate or
SIF build was authorized by that attempt.

## Real layered receipt

The maintained producer was then run against the v32 layered composition:

```text
receipt: Experiments/TigerCluster/results/host-minindn-v32.json
base SIF: sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5
application: sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d
status: PASS
schema: tiger-yolo-host-minindn-manifest-v2
```

The normal case is `minindn-local-20260909-v52-yb49` (v32 APP, exact base
SIF), the permission rejection is the retained v51 Y-N-E `EXPIRED` case, and
the dependency case is the retained v51 Y-N-D run.  The three rows are bound to
their lifecycle, execution/numerical or failure, and cleanup files.  The
dependency row reaches `SELECTION_COMMITTED` and
`PROVIDER_EXECUTION_STARTED`, records two native DetectShard0 withheld tensor
records, and closes with `DEPENDENCY_DATA_MISSING`; it is not the earlier
placement failure.

The v33 dispatch profile consumes this receipt successfully after validating
the exact source seal and the six-artifact `BASE_LIBRARIES_ONLY` native
manifest.  Its qualification remains `NOT_EVALUATED`/`YOLO_HOST_GATE_COMPONENT_ONLY`:
this is a real local CPU/MiniNDN host gate, not a Tiger GPU qualification.
