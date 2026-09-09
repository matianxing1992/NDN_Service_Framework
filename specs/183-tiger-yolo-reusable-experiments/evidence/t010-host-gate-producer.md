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
