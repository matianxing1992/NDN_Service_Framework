# Spec176 capability-selector rewiring evidence

Date: 2026-08-28
Branch: `UAV-Experimental`
Purpose: verify the NDN/data-centric correction that connects the Ground
Station's request-scoped selector to validator-accepted ACK capability data.

## Static and CPU checks

```text
./waf build --targets=unit-tests,UavDroneApp,UavGroundStationApp,App_ServiceController -j1
  passed
./build/unit-tests --run_test=UavTwoLifecycle/DetectorSelectionUsesVerifiedCapabilityAndExplicitFallback --log_level=message
  assertions passed; process exits 139 during libndn-svs static teardown
python3 tests/python/test_uav_collaboration_campaign.py -v
  2/2 passed
git diff --check
  passed
```

The selector now parses `model_id`, `model_digest`, `quality`, `device`,
`ready`, `evidence_access`, `queue_depth`, and `snapshot_ms` from each accepted
ACK candidate and calls `selectUavDetector()`. A provider name alone cannot
make a detector eligible; the explicit Ground Station fallback is considered
only when the fallback profile is enabled and its degraded capability is ready.

The selector assertions pass, but the test process exits 139 during the
libndn-svs static destructor path. The KeyChain-dependent detector test and the
existing certificate test additionally abort in the same system ndn-cxx
`KeyChain::getDefaultKeyParams()`/`__cxa_atexit` path. These are host
runtime/toolchain failures, not selector assertion failures; they are retained
as failed environment gates rather than silently counted as passes.

## Post-wiring MiniNDN attempt

An initial fresh launcher attempt stopped before the controller-ready marker:
`App_ServiceController` printed its authority identity and then crashed in
`CertificatePublisher::findCertificate()` while the current system ndn-cxx
exception/RTTI path handled the certificate lookup. The controller was then
rebuilt with its already-known identity name passed to `CertificatePublisher`,
which avoids that certificate-name exception path.

The candidate-consistent reruns now pass:

```text
nominal MiniNDN: 83.455 s, 2/2 jobs, selector -> /example/uav/drone/C
failure matrix: 10/10 cases PASS, fallback-enabled -> /example/uav/gs
```

Detailed hashes and traces are recorded in
`minindn-nominal-20260828.md` and
`minindn-failure-matrix-20260828.md`. The remaining runtime limitation is the
host unit-test static-teardown segfault; it does not affect these isolated
MiniNDN process results.
