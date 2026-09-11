# CPU integration evidence — Spec183

The integration gate uses the real multi-process NDNSF-DI path and the same
small ONNX input/oracle later sealed into v56. Existing exact-SIF runs v58/v59/
v60 cover the authorized normal graph, permission-denial and post-Selection
missing-dependency cases, empty-HOME/scratch isolation, route/cleanup receipts,
and independent numerical comparison. The v56 local verdict
`sha256:2159969ea07e52265e3147f8c28b2afe3485a078648e21cfbd83a314b3c88e89`
re-executed that graph with the final sealed harness: two requests, four
providers, nine dependency edges/request, CPU execution, shape `[1,50,6]`,
`matched=true`, and controlled cleanup.

The focused integration/producer/dispatch and application/runtime suites also
exercise fresh Controller/epoch binding, denied or stale permissions, wrong
selection and activation/data tamper, duplicate/late output, native cutpoint
identity, and process cleanup. They use real signed message and evidence
schemas; no expected tensor is copied into a distributed result. The test suite
is the component/integration authority, while v56 local and Tiger verdicts are
the runtime qualification authority. MiniNDN evidence is not promoted to GPU
qualification by itself.
