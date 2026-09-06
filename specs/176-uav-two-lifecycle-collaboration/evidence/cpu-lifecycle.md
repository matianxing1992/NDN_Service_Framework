# Spec 176 CPU lifecycle evidence

**Candidate**: `UAV-Experimental` working tree, 2026-08-28

## Configuration

- Compiler: `/usr/bin/g++` selected through `-B/usr/bin`
- Linker/assembler: `/usr/bin/x86_64-linux-gnu-ld` and `as`
- C++ flags: `-O0 -g0 -fno-inline` (used to avoid the previously observed GCC
  9.4 ICE in the large ServiceUser translation unit)
- Build targets: `unit-tests`, `integration-tests`, `UavDroneApp`,
  `UavGroundStationApp`, and `App_ServiceController`
- No GPU, MiniNDN, SITL, or flight claim is made by this fixture.

## Reproduction commands

```bash
env PATH=/usr/bin:/bin:/usr/local/bin:$PATH \
    AS=/usr/bin/x86_64-linux-gnu-as \
    LD=/usr/bin/x86_64-linux-gnu-ld \
    CXXFLAGS='-O0 -g0 -fno-inline' \
    ./waf build --targets=unit-tests,integration-tests,UavDroneApp,UavGroundStationApp,App_ServiceController -j1
./build/unit-tests --run_test=UavTwoLifecycle --log_level=test_suite
./build/integration-tests --run_test=UavCollaborationFlow --log_level=test_suite
```

## Observed result

- Build completed successfully: `unit-tests` and `integration-tests` linked.
- `UavTwoLifecycle`: 15/15 cases passed (including deterministic source
  manifest/lineage binding), process return code 0.
- `UavCollaborationFlow`: 6/6 cases passed, process return code 0.
- The integration case `MissionSessionOutlivesTimedOutRequestAndKeepsStreamsBound`
  proves that a finite request reaching `TIMED_OUT` leaves the long-lived
  MissionSession `ACTIVE`, preserves one completed and one pending mission
  part, and keeps independent video/telemetry stream bindings active.
- The compensation/reconciliation cases prove that a partially completed
  mission marks only the missing part for compensation, preserves the
  completed part's response digest and waypoint progress, and returns the
  MissionSession to `ACTIVE` only after an explicit recovery pass. A timed-out
  physical command is represented as `accepted=false`, `ack=timeout`, with
  `operator-decision` stability handling and no manual replay. This is a CPU
  state-contract regression; it does not claim that telemetry is authoritative
  without a real vehicle source.
- The pressure-isolation case pushes 1,024 detector jobs into a four-item
  queue, observes 1,020 explicit drops, and advances an independent timed-out
  collaboration/command state without changing the MissionSession state.
- The two focused commands were repeated twice after the current rebuild; all
  four invocations returned 0. An earlier diagnostic invocation showed a
  host-library teardown failure, but that failure is not reproduced by the
  current candidate and is not treated as an active Spec 176 blocker.
- The post-audit rerun also covers contextual mission/incident name binding,
  required evidence content type, verified explicit fallback capability, and
  the `ACK_COLLECTING` state before ACK closure.
- The nominal CPU fixture exercises MissionSession independent of a finite
  CollaborationJob, producer-owned versioned evidence names, exact digest
  verification, capability-bound detector selection, one terminal report, and
  fail-closed unready-detector/plan paths, a correlation trace sampler that
  rejects endpoint leakage, and an Interest-driven producer-Data retrieval
  using an exact producer-owned name. The raw-Data adapter also verifies the
  supplied producer certificate and rejects a tampered signature. Only the
  resulting `UavVerifiedEvidence` value reaches detector execution; a
  descriptor or unchecked raw-byte input is no longer an accepted execution
  path.
- The Drone collaboration handler now uses
  `CollaborationContext::fetchSignedExactData()` with an exact evidence name
  and expected producer identity, then admits the returned content only
  through `UavDetectorProvider::acceptValidatedContent()` before execution.
- The multi-segment integration regression additionally exercises
  `CollaborationContext::publishLargeNamed()` and
  `fetchLarge(..., expectedSegments)` over an explicit three-Provider CPU
  DummyFace bridge. It publishes a 12,000-byte producer-owned evidence object
  as 48 signed segments, fetches every segment with exact Interests through the
  configured `MessageValidator`, reconstructs the plaintext twice for two
  consumers, and passes it through `UavVerifiedEvidence` before detector
  execution.

## Boundary

This evidence proves only deterministic in-process application contracts. It
does not prove real NDNSF packet exchange, NAC-ABE authorization, or
  multi-process MiniNDN/PX4 SITL behavior. The current Core candidate routes both
  exact-segment and SegmentFetcher large-data retrieval through the configured
  `MessageValidator`/ndn-cxx trust-schema validator. The deterministic
  multi-segment CPU regression is recorded in
  `evidence/multisegment-evidence-20260828.md`; packet-loss/reordering and SITL
  deployment evidence remain separate gates.

The full 611-case unit suite was also attempted. It reached the existing
`NativeTensorBundleCodecRoundTripsPilotDtypesDynamicShapesAndKvOutputs` case,
which terminates with an integer divide-by-zero before completing; this is a
pre-existing distributed-inference failure outside the Spec 176 UAV slice and
is retained as a baseline blocker rather than hidden by the focused gate.

The UAV application binaries were rebuilt successfully. With
`LD_LIBRARY_PATH=build:build/NDNSF-DistributedRepo:/usr/local/lib`, `ldd`
resolves `libndn-service-framework.so.0.1.0` to the candidate build and no
undefined-symbol error occurs. A direct no-NFD startup is not an acceptance
pass: the drone exits with the expected “runtime did not become ready” message
and the Ground Station remains event-loop bound, so real deployment remains a
MiniNDN/SITL gate.

The fixed MiniNDN launcher passes read-only preflight (all four binaries,
configuration, trust schema, topology, and Python MiniNDN import). A rootless
isolated-network nominal run and the ten-case failure matrix now have separate
candidate-bound evidence; PX4 SITL remains unverified because its scenario
adapter is not present.
