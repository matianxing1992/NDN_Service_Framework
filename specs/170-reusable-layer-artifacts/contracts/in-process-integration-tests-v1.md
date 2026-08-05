# Spec 170 In-Process Integration-Test Contract (v1)

**Status**: implemented baseline pre-freeze test layer with a reusable
preconfigured environment fixture; this document does not
close Gate A or replace the real MiniNDN Gate B.

## Purpose

The current `unit-tests` target checks individual contracts. Spec 170 also needs
a deterministic packet-level integration layer that composes the real NDNSF
encoders, faces, handlers, token checks, artifact framing, and SVS control/data
path in one process. This layer should run before MiniNDN and find most wiring,
serialization, name, replay, deadline, and partial-output defects without an
NFD, network namespace, model download, or GPU.

The layer is evidence for protocol and lifecycle correctness only. It does not
claim to prove NFD routing, socket/namespace isolation, process crashes, real
certificate deployment, CUDA/NCCL behavior, Slurm allocation, or MiniNDN
multi-process timing. Those remain Gate B/C/D obligations.

## Four-level ladder

| Level | Fixture | Finds | Does not prove |
|---|---|---|---|
| L0 | Existing Python/C++ unit fixtures | Pure schema, digest, mutation, and state-machine errors | Cross-component packet wiring |
| L1 | Two or more real NDNSF runtimes over `ndn::DummyClientFace` | Interest/Data names, TLV round trips, ACK_CLOSED -> Selection -> Response, tokens, replay, deadline, and response completeness | NFD routing and process isolation |
| L2 | Three or more `DummyClientFace` nodes with real `SVSPubSub`/`SVSync` | Mapping, piggyback, fetch, repair, duplicate suppression, segmentation, cancellation, and `NDNSF_DATA_V1` framing | Kernel/network loss and production NFD behavior |
| L3 (optional) | Real binaries with an explicit loopback/in-process transport adapter | Executable wiring, configuration, and cross-thread shutdown | NFD topology, namespaces, and MiniNDN deployment |
| L4 | Existing MiniNDN Gate B | Real routing, security/process lifecycle, Repo, and deployment-faithful cold/warm workload | TigerCluster hardware unless run there |

L1 and L2 are the requested integrated tests. They are now a separate C++
`integration-tests` target so a failing link or unavailable NFD does not
silently turn them into unit tests. The Python cross-module flow suite runs
alongside it for the V3/DI-specific contracts that are owned by the Python SDK.

## Harness contract

### Face and packet bridge

Use the ndn-cxx `ndn::DummyClientFace` rather than a host NFD. The simple case
may call `faceA.linkTo(faceB)` and inspect `sentInterests`, `sentData`, and
`sentNacks`. For negative cases, use a small `PacketBridge` that forwards
packets through `DummyClientFace::receive(...)` and applies an explicit
drop/duplicate/reorder rule. This keeps faults deterministic and avoids
pretending that a broadcast dummy link is an NFD route table.

Each test owns its faces and identities. A `PacketTrace` records, at minimum:

```text
direction, packet kind, full Name, parameters digest, requestId,
planCoreDigest/planDigest, provider, sequence/segment, timestamp
```

Assertions compare complete payload bytes and decoded fields, not payload size
or log substrings. The bridge must expose counters for forwarded, dropped,
duplicated, and late packets.

### Reusable preconfigured environment fixture

The request cases use the same fixture shape as ndn-cxx tests: the fixture
owns the faces, memory KeyChain, identities, SVS nodes, packet bridges, and
real User/Provider runtime objects, while each case owns only a request scope.
The Spec 170 implementation is
`tests/integration-tests/ndnsf-integration-fixture.hpp/.cpp`:

```cpp
NdnsfIntegrationEnvironment env;
env.bootstrap();                         // NEW -> READY, measured separately
auto request = env.beginRequest("req-1"); // READY -> REQUEST_ACTIVE
env.markRequestPublished(request);       // request trace boundary
// exercise a request using env.user(), env.provider(), and env.*PubSub()
env.updateRequestResidue(request, {});    // no lease/device/callback/replay/plaintext
env.resetRequest(request);                // REQUEST_ACTIVE -> READY
```

`bootstrap()` installs the memory-backed User/Provider permissions and checks
one signed SVS publication over the real `DummyClientFace` bridge before
emitting the snapshot digest. `beginRequest()` refuses to run before `READY`,
and `resetRequest()` refuses to close a scope before the explicit
`REQUEST_PUBLISHED` boundary. The snapshot digest is carried by every case;
mutable token, replay, queue, lease, device, callback, and response state must
remain request-local. The fixture's bridge now applies request-local
drop/duplicate/pairwise-reorder faults to real Interest/Data forwarding and
reports counters plus the first dropped/pending name; `resetRequest()` refuses
to close while a reorder hold remains. `BootstrapProfile.providerCount` also
constructs additional real Provider runtimes, faces, SVSPubSub nodes, and
permission tables; the integration target verifies a three-Provider READY
boundary. The complete three-Provider request projection, corruption/replay,
and protected `NDNSF_DATA_V1` cases remain open T025 work. The existing smoke
cases remain compatibility coverage until they are migrated onto it.

### Bounded event loop

Every test uses a bounded pump. The pattern used by ndn-svs tests is the model:
restart the face I/O context, run it for a small slice, and stop at a monotonic
deadline. Never call an unbounded `processEvents()` in CI.

```cpp
static void
runIoUntil(ndn::Face& face, const std::function<bool()>& done)
{
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    face.getIoContext().restart();
    face.getIoContext().run_for(10ms);
    std::this_thread::sleep_for(1ms);
  }
  BOOST_REQUIRE_MESSAGE(done(), "in-process integration deadline expired");
}
```

For a multi-face fixture, pump every face in a round-robin loop and include the
face name in timeout diagnostics. Use the project test clock/deadline
abstractions where the component supports them; otherwise use one monotonic
deadline and never sleep for a protocol timing claim. SVS options must retain
the production protocol/suppression defaults unless a test explicitly covers a
documented option.

### Security and identity

Use the real signing/verifying and token code with memory-backed test
identities. A test double may provide a deterministic certificate or clock, but
must not bypass authorization (`isAuthorized = true`) or replace NAC-ABE,
UserToken, ProviderToken, replay, or revocation checks. Every negative test
asserts both the terminal error and the absence of a later Selection,
Response, plaintext protected artifact, or partial output.

## Test families for Spec 170

### L1 request lifecycle

1. Create one Requester and three Providers with real generic runtime handlers
   on linked dummy faces.
2. Publish a V3 request and collect ACKs until the configured ACK window
   closes. Decode the ACK snapshot and pass it through the real
   `PlanSealerV3` and Provider-specific Selection projection.
3. Forward only the selected Selection. Providers verify the exact
   `ProviderToken`, fencing/plan digests, and request ID; the selected Provider
   executes and returns a complete Response.
4. Assert no ACK-time reservation/lease, no non-selected Response, no V2
   fallback, and no state change after a duplicate/replayed Selection.

Recommended cases:

| Case | Fault | Required observation |
|---|---|---|
| `V3OfferSelectionResponse` | none | ACK_CLOSED, one valid Selection, one complete Response |
| `RejectDoesNotReserve` | contradictory offer tuple | rejection before ACK_CLOSED; zero queue/lease side effects |
| `SelectionReplayAndStalePlan` | duplicate or stale digest | no execution and no second Response |
| `CustomSelectionTimeout` | delayed ACKs | strategy runs once after timeout with full status/message/payload matching |
| `ThreeProviderConcurrency` | three simultaneous requests | independent request IDs, single-flight per artifact, no cross-request token use |

### L1 canonical artifact and reuse

Connect the real canonical publisher, segment fetcher, Provider assembly, and
runtime catalog to an in-memory content-addressed store behind the dummy-face
bridge. Publish manifest/index/object Data in root-last order and verify every
wire digest and signature before activation.

Cases must cover:

- cold selective retrieval of the required layer set;
- duplicate publication and concurrent single-flight publication;
- missing, truncated, wrong-digest, wrong-shape, wrong-ABI, and wrong-signer
  objects, each leaving no active temporary output;
- exact warm reuse returning zero model-byte transfer, zero assembly, and zero
  reload;
- V2 names/bytes never decoding as V3 and no request-dependent filesystem path
  derived from an NDN name.

### L2 SVS and `NDNSF_DATA_V1`

Instantiate one real `SVSPubSub` per producer/receiver with separate
`DummyClientFace` instances and a bounded packet bridge. Subscribe the receiver
to the NDNSF application prefix, publish a signed/encrypted
`NDNSF_DATA_V1` segment stream, and drive the normal mapping, fetch, and repair
callbacks. Exercise both piggyback and explicit repair; use the same sequence,
segment, epoch, nonce, AEAD/HMAC, and duplicate rules as the production path.

Required cases:

- in-order, out-of-order, duplicate, and replayed segments;
- dropped segment followed by repair and a no-progress deadline;
- wrong epoch/nonce/tag/signature and plaintext-wire negative cases;
- cancellation while a segment is in flight;
- missing rank/provider and late data, proving whole-operation failure and zero
  downstream partial output;
- repeated mapping or repair does not invoke the subscriber twice.

The ndn-svs `SVSPubSub` layer is the right integration boundary: it exercises
publication, subscription, mapping, piggyback, and repair while retaining the
real `SVSync`/`SVSyncCore` state machine. Do not test only `SVSyncCore` and call
that an end-to-end result.

### L2 protected-artifact lifecycle

Use real policy/key-grant and `PlaintextLeaseRegistry` implementations with a
memory authority and the same dummy-face transport. Trace:

```text
GrantRequest -> signed KeyGrant -> unwrap -> lease -> JIT admission
-> revoke/expiry -> cancellation -> zeroization -> ZEROIZED
```

Inject wrong recipient, wrong `planCoreDigest`, incomplete grant cover, stale
revocation state, nonce reuse, lease-registry omission, and zeroization failure.
The expected result is a narrow terminal failure and no Selection or plaintext
artifact activation.

## C++ layout and commands

Keep the harness beside the existing tests rather than embedding it in Python:

```text
tests/integration-tests/
  ndnsf-di-core-flow.t.cpp           # plan, dataflow, waits, evidence cases
  (tests/main.cpp)                     # Boost.Test entry point
```

The current SVS packet fixtures remain in
`tests/unit-tests/ndn-svs-smoke.t.cpp` and are linked into the separate target;
the file split above is the next cleanup once the fault-injection bridge is
added.

The Waf declaration links the same production libraries as `unit-tests`, plus
the native DI core sources used by `ndnsf-di-core-flow.t.cpp`. The current
commands are:

```bash
./waf configure --with-tests
./waf build --targets=integration-tests
build/integration-tests --run_test='NdnSvsSmoke/*'
build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/*'
LD_PRELOAD=results/spec170-native-overlay-20260805T061000Z/libndn-service-framework.so.0.1.0 \
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
pytest -q tests/python/test_spec170_integrated_flows.py
```

The baseline currently contains two SVS/NDNSF packet cases, nine native
DI/fixture cases (including deterministic drop/duplicate/reorder bridge
coverage, a three-Provider bootstrap case, a generic Request/ACK/Response
case, and a timeout-driven three-Provider custom-selection case), and five
Python cross-module cases. Run L1/L2 on every local Gate A pass.
Preserve the command, source/library
hashes, packet trace, complete negative rows, and timeout diagnostics in
`specs/170-reusable-layer-artifacts/evidence/gate-a.md`. A passing integrated
test layer improves Gate A coverage; it does not mark Gate B PASS or authorize
TigerCluster submission.

## Acceptance criteria

- At least one positive and one negative case exists for each L1/L2 family.
- Tests use production encoders, decoders, handlers, token/security checks,
  artifact framing, and SVS callbacks; only transport, clock, and storage
  boundaries are deterministic fixtures.
- Every case has a bounded deadline and reports the first missing packet/event.
- Full names, parameters digests, payload bytes, signatures, and sequence/
  segment identities are compared.
- Duplicate, replay, cancellation, timeout, and malformed-data cases prove no
  partial output and no hidden lease/reservation.
- The report labels L1/L2 as in-process evidence and keeps MiniNDN/TigerCluster
  as the required deployment/hardware gates.

## Implemented baseline case matrix

| Case group | Current cases | Main flow covered |
|---|---|---|
| `NdnSvsSmoke` | `DummyFacesDeliverV2RequestPublication`, `ServiceUserRequestServiceReachesProviderAndReturnsResponse` | DummyFace forwarding, SVS publication, generic Request/ACK/Response, typed callback |
| `Spec170NdnsfDiCoreFlow` | `AttemptPlanAndDependencyNamesRemainBound` | attempt epoch, stale/cancel/terminal fencing, role/dependency naming |
|  | `AsyncDataflowRunsThreeStagePipelineAndRejectsMissingOutput` | three-stage dependency-driven execution and provider failure |
|  | `DependencyWaitCoversCompletionCancellationAndDeadline` | completion, cancellation, deadline terminal states |
|  | `ExecutionEvidenceRoundTripsAndRejectsSecrets` | evidence serialization and secret-field rejection |
|  | `PreconfiguredEnvironmentSeparatesBootstrapFromRequests` | explicit bootstrap/READY/request/reset boundary and residue rejection |
|  | `PreconfiguredEnvironmentAppliesDeterministicPacketFaults` | signed Data drop, duplicate, pairwise reorder, counters, and reset guard |
|  | `PreconfiguredEnvironmentBootstrapsThreeProviders` | three real Provider faces, SVSPubSub nodes, identities, permissions, and indexed access |
|  | `PreconfiguredEnvironmentRunsGenericRequestLifecycle` | fixture Request → SVS publication → Provider handler → ACK → Response → typed callback |
| Python `test_spec170_integrated_flows.py` | V3 lifecycle, canonical/assembled reuse, DATA_V1, protected grant/lease, multi-device | ACK_CLOSED → Selection → JIT admission, artifact root-last/assembly, epoch/MAC/replay, revocation/zeroization, atomic device admission |

This is a broad pre-MiniNDN contract net, not a claim that all deployment
behavior is covered. The remaining required expansion is explicit packet fault
injection for multi-Provider `NDNSF_DATA_V1` over the bridge and real Repo/
security/process behavior in MiniNDN Gate B.
