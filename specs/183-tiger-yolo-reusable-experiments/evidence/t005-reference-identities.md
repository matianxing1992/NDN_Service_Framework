# T005/T006 independent reference identity correction

Date: 2026-09-07. Scope: focused implementation repair, not runtime qualification.

## Source finding

`Yolo26Splitter._candidate` builds `fragments_by_role` from the candidate,
planning graph, role and node-cover contract. `AutomaticPlanningCoordinator`
copies that logical digest into `RoleAssemblySpec.artifact_digest`; the public
assignment projection and retained comparator use the same logical identity.

`prepare_role_reference` instead required `sha256(model_bytes) == artifact_digest`.
Consequently a real certified role and its assembled ONNX bytes could not be
passed together, while old fixtures hid the problem by using the byte digest
for both meanings. CodeGraph and exact source confirmed the two owners.

## Change and evidence

The reference API now takes both `artifact_digest` and `assembled_model_digest`.
It verifies the latter against the supplied bytes, retains the logical identity
for Selection comparison, and seals `assembledModelDigest` into reference
provenance. Serializer and retained provenance validation require that field.
Existing component documents lacking it fail closed and must be regenerated.
No wire protocol, native library or model implementation changed.

Red: the new logical-versus-byte identity test failed on the original API
(`unexpected keyword argument 'assembled_model_digest'`). Green: real tiny CPU
ONNX preparation accepts distinct logical and byte identities; wrong model
bytes and malformed logical/assembled/provenance digests are rejected. Existing
native observation and four-role/three-ORT join cases remain covered.

```bash
python3 -m pytest -q \
  Experiments/TigerCluster/tests/test_yolo_graph_reference.py \
  Experiments/TigerCluster/tests/test_yolo_native_observation.py \
  Experiments/TigerCluster/tests/test_yolo_retained_execution.py \
  --junitxml=Experiments/TigerCluster/results/spec183-reference-identities-20260907/focused.xml
```

Result: **145 passed in 5.52s**. The JUnit file is retained locally outside Git.
Only affected reference/comparator tests ran; no full historical suite, native
build, MiniNDN, SIF qualification or Tiger job was launched by this repair.

## Remaining production boundary

T005/T006/T007 remain open. `apps/yolo.py` still lacks the real producer call;
the public collector still needs to bind the document to that producer rather
than arbitrary caller-supplied JSON. A second source finding constrains that
work: `YoloCanonicalArtifactBinding.ensure` updates `model_manifest_digest`
after publishing the request's canonical MODELROOT, and placement re-certifies
role specs immediately afterward. That digest is not the offline package
manifest hash. Independent graph vocabulary can be prepared from certified
model bytes, but the final reference must bind the actual request manifest.
Do not guess that identity in offline prepare, call private placement methods
as a substitute for a real request, or copy observed node assignments into the
expected graph. CUDA references still belong on allocated devices.

The existing host build was observed live, then its original PIDs disappeared;
a replacement build tree was confirmed live under another client (PID 1184413,
with compiler child 1185727 at the observation). This agent did not start,
restart, terminate or modify that build. Its completion is still unverified.
