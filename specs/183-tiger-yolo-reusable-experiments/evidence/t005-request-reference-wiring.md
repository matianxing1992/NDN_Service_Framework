# Per-request independent reference wiring — 2026-09-07

Status: IMPLEMENTED; focused component checks pass. T005/T006 remain open and
T007 remains BLOCK. No native application, complete YOLO, SIF or GPU PASS.

## Production connection

`apps/yolo.py::run_requests` launches the internal `user` command, composing
the installed example User with a trusted Python factory for the existing
`YoloCanonicalArtifactBinding`. Ordinary example invocation retains the
original owner. No new protocol, planner, certification or service mode.

`RequestReferenceBinding.ensure` receives the coordinator's post-ACK certified
role specs before Selection. `CertifiedOnnxAssemblyRecipe.from_role_spec`
reconstructs and validates the existing recipe. The public DI assembler uses
the package's hash-checked ONNX and initializer bytes. Three model roles then
initialize independent ORT reference sessions; they never run inference or
read Provider observations. Merge remains native CPU. Plaintext optimized
models stay in private temporary files and are removed.

After the existing owner publishes MODELROOT, its resulting manifest digest
is bound into the independent expectations. The User writes digests and node
identities to `user/requests/<index>/graph-reference.json`, including run,
request, runtime and placement identities. Role/recipe/source/deadline failures
prevent reference publication. A second ensure is rejected; normal Spec183
has no reselection. CUDA preparation uses the allocated node-0 User container
with `--nv` only after allocation/GPU preflight, never the login node.

`collect_normal_verdict` reads each request's retained reference before
collecting its native observations and validates all identities/provenance.
A caller-supplied shared graph cannot replace a missing User record. Different
request-time MODELROOT digests remain separate. This supersedes the earlier
offline `prepare_in_container` / `public/certified-graph.json` proposal.

## Verification and remaining work

One external-initializer fixed vector uses the real DI assembler and CPU ORT;
identity mutations reuse the resulting record. Application command/process
tests use explicit fixtures, not native execution. Annotation-only native
imports in executor/splitter/graph are delayed; converting a graph edge to
an InferenceDependency still imports the actual runtime at that boundary.

Command:

```bash
python3 -m pytest -q --tb=short \
  Experiments/TigerCluster/tests/test_yolo_request_reference.py \
  Experiments/TigerCluster/tests/test_yolo_application.py \
  Experiments/TigerCluster/tests/test_yolo_worker.py \
  Experiments/TigerCluster/tests/test_yolo_retained_execution.py \
  Experiments/TigerCluster/tests/test_yolo_collection.py \
  tests/python/test_spec180_yolo_numerical.py \
  --junitxml=Experiments/TigerCluster/results/spec183-request-reference-wiring-20260907/component.xml
```

**176 passed in 18.51s.** An earlier combined command also included
`tests/python/test_spec180_yolo_application.py`, but collection failed:
`/usr/local/lib/libndn-service-framework.so.0.1.0: undefined symbol:
_ZN5ndnsd9discovery16ServiceDiscoveryD1Ev`. Its failed JUnit remains `focused.xml`
in the same directory. No failed result was overwritten or counted as PASS.
That check awaits host build/loader closure. Another client's running build
was neither changed nor restarted.

Next: close the actual operator execution/staging seams, validate installed
User composition with the qualified native closure, and re-audit T007. Source
changes invalidate the older source seal; only a freshly sealed, locally
qualified SIF may advance to the bounded Tiger GPU gates.
