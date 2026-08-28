# Native `NDNSF_DATA_V1` primitive (2026-08-16)

This checkpoint implements and tests the first native cross-Provider data
primitive.  `NdnsfCollectiveControl` authenticates a bounded segment descriptor
and Data name with AES-256-GCM plus a domain-separated HMAC.  The descriptor
binds the request, attempt, plan, group, epoch, operation, tensor/manifest
digests, producer rank, segment bounds, and progress/deadline values.
`DataSegmentReplayWindow` authenticates before mutating state, makes an exact
duplicate idempotent, rejects conflicting duplicates, and enforces segment and
byte limits.

Verification:

```text
./build/unit-tests --run_test=DistributedInferenceCrossProviderGroup --log_level=test_suite
7 test cases; *** No errors detected

./build/integration-tests --run_test=NdnsfDataV1SvsFlow --log_level=test_suite
1 test case; *** No errors detected
```

The integration fixture uses three independent `DummyClientFace`/SVSPubSub
instances.  Two Provider streams publish independently addressable
`NDNSF_DATA_V1` segment bundles out of order; the receiver maps them through
SVS, drops one Provider segment once, observes the repair/retry, reassembles
all four segments, and rejects a replayed duplicate.  This is real in-process
SVSPubSub/face traffic, not a direct function-call loopback.

The source object and unit-test binary linked with the host ORT 1.26.0 tree
through the existing `/tmp/ndnsf-ort120 -> /opt/onnxruntime-1.26.0` build
alias.  The full Waf build still has an unrelated host GTK/GLib UAV-link
failure; this primitive test does not depend on UAV targets.

The unit fixture also covers RSA-wrapped epoch-key recovery, capability
validation, manifest mutation rejection, deadline/terminal-state handling,
and the bounded wire bundle.  The segment bundle is now independently
fetchable through the SVS mapping path.

This is still not the complete Spec 170 T017/T018/T033 implementation.  The
current bridge publishes the segment bundle through the existing large named
Data path; it does not yet replace the production collaboration transport with
per-segment raw SVS publication.  Production NDN KeyChain manifest signing,
ciphertext (rather than plaintext) segment-digest binding, and a real
cross-Provider native workload remain before D2b can be reopened.  The result
therefore upgrades local L2 evidence only; it is not a TigerCluster pass.
