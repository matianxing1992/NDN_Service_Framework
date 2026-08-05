# Spec 170 Preconfigured In-Process Environment (v1)

## Decision

Integration cases should not reconstruct the entire NDNSF system before every
request. Build one deterministic, fully configured environment per test
process, run initialization/bootstrap as a separate test lane, and start every
request case from the persisted `READY` boundary.

This is a test-environment optimization, not a protocol shortcut. The
bootstrap lane must exercise the same identity, permission, SVS, Repo, catalog,
security, and provider wiring that a request will use. Request cases may reuse
immutable configuration and keys, but never share mutable request state.

## Phase model

```text
NEW
  -> BOOTSTRAPPING
  -> READY(snapshot)
  -> REQUEST_ACTIVE(requestScope)
  -> READY(snapshot)

BOOTSTRAPPING -> FAILED(diagnostic)
REQUEST_ACTIVE -> READY(snapshot)   # complete/cancel/cleanup only
```

The environment exposes an explicit status and refuses request submission until
`READY`. A bootstrap failure is a bootstrap failure; it is not converted into a
request timeout.

### Immutable environment state

Construct once and bind it to an `EnvironmentSnapshot`:

- ndn-cxx `DummyClientFace` packet bridges and bounded I/O pump;
- memory-backed identities, certificates, trust/policy configuration, and
  permission epoch;
- Controller/User/Provider permission installation and authorization tables;
- SVS sync prefix, producer nodes, protocol options, and registration state;
- content-addressed Repo/catalog roots and immutable canonical layer objects;
- adapter/model/profile identity and native/Python contract version;
- deterministic clock seed, fault-policy seed, and configuration digest.

The snapshot records hashes and readiness markers, not secret key material.
Every request trace carries the snapshot digest so a result cannot be confused
with a different bootstrap configuration.

### Per-request transient state

Create and destroy for every case:

- request ID, attempt epoch, user/provider tokens, ACK_CLOSED snapshot;
- plan core/final digest, Selection projections, queue/JIT records, fencing
  token, dependency bitmap/inflight state;
- protected-artifact grant/lease/revocation handles;
- response buffers, cancellation/deadline state, and packet trace.

`resetRequest()` must prove zero live leases, zero held devices, no pending
interests/data callbacks for the request, no replay-window entries, and no
plaintext artifact before returning the environment to `READY`.

## Fixture API

The C++ fixture should have an explicit two-lane API rather than hiding setup in
the first request test:

```cpp
class NdnsfIntegrationEnvironment {
public:
  void bootstrap(const BootstrapProfile& profile); // NEW -> READY
  EnvironmentSnapshot snapshot() const;
  RequestScope beginRequest(std::string requestId,
                            FaultProfile faults = {}); // READY -> ACTIVE
  void updateRequestResidue(RequestScope&, RequestResidue); // harness cleanup state
  void pumpUntilReady();
  void resetRequest(RequestScope& scope);              // ACTIVE -> READY
  EnvironmentStatus status() const;
};
```

`BootstrapProfile` is immutable after `bootstrap()`. `FaultProfile` is
request-local and can drop, delay, duplicate, reorder, corrupt, or replay
packets/data for one case. The bridge must expose counters and the first missing
event in a timeout diagnostic.

The implemented fixture is
`tests/integration-tests/ndnsf-integration-fixture.hpp/.cpp`. The existing SVS
fixture wrappers in `tests/unit-tests/ndn-svs-smoke.t.cpp` remain compatibility
coverage, while new L1/L2 cases should use the reusable environment fixture.
The current baseline instantiates one User/Provider pair by default and applies
the request-local `FaultProfile` through a deterministic bridge. Drop,
duplicate, and pairwise reorder forwarding expose counters and the first
dropped/pending name; `resetRequest()` rejects an unflushed reordered packet.
`BootstrapProfile.providerCount` can create additional real Provider
DummyFaces, SVSPubSub nodes, identities, and permission tables; the integration
target now verifies a three-Provider READY/request boundary and a timeout-driven
custom-selection response projection. The full L1/L2 request matrix,
corruption/replay rules, and protected `NDNSF_DATA_V1` cases are still
follow-up work, not silently counted as implemented coverage.

## Bootstrap lane

Run exactly once per integration-test process (and independently in a dedicated
bootstrap test job when measuring initialization):

1. Create the in-memory identities/certificates and verify the trust/policy
   configuration digest.
2. Wire DummyFace bridges and real `SVSPubSub` nodes; verify registration and
   one signed publication/repair round trip.
3. Install User/Provider permissions and verify one authorized request can be
   admitted; verify a wrong identity is rejected.
4. Publish/verify the canonical root and assembled artifact catalog; verify
   root-last and idempotence.
5. Verify native/Python profile, adapter, route, and security-policy digests
   match the snapshot.
6. Emit `NDNSF_INTEGRATION_BOOTSTRAP_READY` with the snapshot digest.

The bootstrap lane is not included in request latency or TTFT. Its own timing,
failure, and readiness evidence is retained separately.

## Request case matrix

Every case starts after the same `READY` marker and uses a fresh request scope.

| Case | Expected starting point | Main assertion |
|---|---|---|
| `cold_v3_request` | READY + empty transient cache | ACK_CLOSED → V3 Selection → JIT admission → complete Response |
| `warm_v3_reuse` | READY + immutable canonical/assembled catalog | zero canonical model-byte transfer/assembly/reload |
| `invalid_offer_and_stale_selection` | READY | fail closed; no queue hold, lease, device hold, or Response |
| `three_provider_custom_selection` | READY | timeout-driven strategy selects only selected Providers |
| `cross_provider_data_v1` | READY | signed/encrypted segment stream, repair, duplicate/replay rejection, whole-operation failure |
| `protected_revoke_cancel` | READY | grant → lease → revoke/expiry → cancellation → zeroization |
| `multi_device_admission` | READY | independent device allocation, atomic busy-device rejection, no ACK reservation |
| `deadline_and_no_progress` | READY | bounded terminal evidence and zero partial output |

Cases may use a warm immutable catalog, but they must declare whether the
request is cold or warm. They must never inherit an unfinished request's lease,
token, replay window, queue entry, or response buffer.

## MiniNDN relationship

This environment catches most protocol/control/lifecycle defects before
MiniNDN. It does not prove NFD routing, process/namespace isolation, kernel
transport loss, or real deployment certificate stores. Gate B still starts the
deployment-faithful MiniNDN system and uses the same configuration/profile
digests; only its process/network boundary changes.

The MiniNDN harness should expose the same semantic markers:

```text
NDNSF_INTEGRATION_BOOTSTRAP_READY <snapshotDigest>
NDNSF_REQUEST_PUBLISHED <requestId> <snapshotDigest>
NDNSF_REQUEST_TERMINAL <requestId> <status>
```

This makes bootstrap failures and request failures comparable without treating
the initialization phase as a request failure.

## Acceptance criteria

- A dedicated bootstrap case passes and emits a snapshot digest.
- Every request case refuses to run without `READY` and begins its measured
  trace at `REQUEST_PUBLISHED`.
- Reset returns the environment to `READY` with zero request-scoped leases,
  held devices, replay entries, pending callbacks, and plaintext files.
- At least one positive and one negative case covers each major Spec 170 flow.
- Case results include snapshot digest, request/attempt ID, fault profile, and
  terminal evidence; bootstrap time is reported separately.
