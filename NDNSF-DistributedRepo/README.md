# NDNSF-DistributedRepo

NDNSF-DistributedRepo is an experimental C++ subproject for a distributed,
NDN-native, policy-controlled artifact and intermediate-data storage layer for
NDNSF applications.

The current implementation provides the reusable C++ storage API and the
standalone C++ Persistent network service. Python exposes a thin binding over
the same C++ artifact API:

- `RepoObjectManifest`: signed-manifest-ready metadata for one stored object.
- `StorageCapability`: advertised capacity/load/mode information for a repo
  node.
- `RepoCatalogStatus`, `RepoCatalogEntry`, and `RepoCatalogDelta`: logical
  object-level catalog metadata for replica discovery and recovery.
- `PlacementPolicy`: controlled replication requirements.
- `selectReplicas`: deterministic placement over candidate storage nodes.
- `RepoCore`: reusable in-process storage logic with no dependency on NDNSF
  networking.
- `RepoNode`: the C++ Persistent service owner and trusted local adapter.
- `DistributedRepoServer`: the standalone C++ production service entrypoint.
- `ArtifactRepositoryApi`: the native C++ In-App and Persistent artifact API.
- `py_repoclient`: a thin pybind projection of `ArtifactRepositoryApi` and its
  native value/config/result types.

The Persistent service prefix is configurable. By default, Repo nodes use:

```text
/NDNSF/DistributedRepo/Object/v1
```

The standalone C++ `DistributedRepoServer` registers the Persistent network
handlers under this prefix. C++ `RepoNode` also exposes trusted local services
through `LocalServiceRegistry`. The repo-level operation is `INSERT`: an application publishes signed and optionally
encrypted NDN Data segments, and the repo stores those opaque Data packets by
name. `STORE` and `STORE_MANIFEST` are current payload-accepting adapters used
by existing helpers and tests. They are input conveniences: the caller may hand
the API payload bytes, but the repo/client adapter should still name, segment,
and sign those bytes as NDN Data and then use the same segment storage
semantics. They are not a separate "small object" repo model. Current C++
services include:

```text
CAPABILITY
STORE
INSERT
STORE_MANIFEST
FETCH
MANIFEST
INVENTORY
STATUS
CATALOG_STATUS
CATALOG_SNAPSHOT
CATALOG_DELTA
CATALOG_LOOKUP
CATALOG_QUERY
DELETE
```

Python/NDNSF-DI helpers should move toward `INSERT` for app-owned segmented
Data packets. If an application only has raw payload bytes, the payload should
first be converted into the same Core large-data representation: hybrid
AES-GCM encrypted, signed segmented Data plus a small reference/manifest. The
repo stores opaque Data packets and metadata; it is not a separate large
payload transport.

Objects are described by `RepoObjectManifest`, which contains object name,
object type, SHA-256, size, segment count, replication factor, selected replica
nodes, and policy epoch. The manifest is metadata that maps an application
object name to stored Data segments and hash information; it is not a second
payload transport. Placement and replica selection use NDNSF service discovery
and ACK metadata. This keeps the repo generic: the stored Data may represent a
model shard, runner, ONNX file, PyTorch artifact, activation tensor,
payment-workflow record, telemetry log, JSON configuration, or any other NDNSF
application object.

## App-Owned Segmented Data References

For large objects, the preferred NDN-native path is for the application or Core
runtime to publish hybrid-encrypted, signed segmented Data under its own
namespace. The repo request then carries only a `RepoDataReference`: object
name, Data prefix, optional segment range/final segment hint, forwarding hint,
expected size, and expected SHA-256. The repo fetches those Data packets
through an injected SegmentFetcher adapter. Each fetched packet is decoded only
to obtain and validate its complete Data name, such as
`/data/model/v=42/seg=0`; that exact name is the authoritative storage key and
the complete signed wire is stored byte-for-byte. The logical object manifest
contains the ordered `packetNames` index. The packet path never creates
`<objectName>/ndn-data/<N>` or `<objectName>/seg/N` aliases.

The repo does not decrypt, reinterpret, or re-authorize the application data.
It only stores opaque Data wire packets and reports operation status:

```text
/NDNSF/DistributedRepo/INSERT
/NDNSF/DistributedRepo/STATUS
```

`STATUS` returns a `RepoOperationStatus` with states such as `FETCHING`,
`STORING`, `DONE`, and `FAILED`. If a standalone repo node has not configured a
SegmentFetcher adapter yet, `INSERT` fails visibly instead of
pretending that remote Data was fetched.

## In-App and Persistent Repo Modes

NDNSF-DistributedRepo distinguishes two deployment roles. An **In-App Repo** is
embedded in the same trusted process as an NDNSF application. It is useful for
low-latency local cache, temporary intermediate artifacts, recording metadata,
and fast same-process helper access. It is not the default backup target for
long-term data. A **Persistent Repo** runs as an independent repo node or
server. It is the normal target for backup, durable storage, global catalog
maintenance, and recovery after a node joins late or misses catalog updates.

The Python `ArtifactRepositoryApi` delegates to the native C++ API. Use
`make_in_app(...)` for an app-owned durable root and `make_persistent(...)` for
an existing authorized `ServiceUser`. Construct separate API instances when an
application needs both paths. Python backend injection and Python-owned local or
network lifecycle state are retired.

`DistributedRepoServer` is the standalone Persistent service. In-App storage is
an `ArtifactRepositoryApi` instance and does not register a network Repo node.
Python publication returns `ArtifactPublishResult` with an immutable
`ArtifactHandle`; retrieval returns `ArtifactFetchResult`. Archiving that handle
to Persistent storage is an explicit operation.

The C++ `RepoDeploymentMode` parser retains legacy values for source
compatibility; those values do not alter the standalone server's Persistent
role. The separate `StorageCapability.acceptsBackupReplica` field controls
whether a candidate may be selected for backup placement.

## Catalog and Replica Discovery

The catalog follows the useful HDFS idea of separating object-location metadata
from stored bytes, but keeps the NDN design decentralized. There is no single
NameNode. Every repo maintains its own local catalog, and Persistent Repos can
exchange object-level catalog deltas to build a replicated global catalog.

Catalog entries describe logical objects and manifests, not every segment:

```text
objectName
manifestSha256
objectType
size
segmentCount
sourceRepo
repoMode
state
catalogEpoch
replicaNodes
manifest
```

The current service/API surface is:

```text
/NDNSF/DistributedRepo/CATALOG_STATUS
/NDNSF/DistributedRepo/CATALOG_SNAPSHOT
/NDNSF/DistributedRepo/CATALOG_DELTA
/NDNSF/DistributedRepo/CATALOG_LOOKUP
/NDNSF/DistributedRepo/CATALOG_QUERY
```

`CATALOG_STATUS` returns the repo node, mode, current catalog epoch, object
count, and whether the repo accepts backup replicas. `CATALOG_SNAPSHOT` returns
a full object-level snapshot for recovery. `CATALOG_DELTA` returns only changes
after a caller-provided epoch. `CATALOG_LOOKUP` returns the catalog entry for
one object name. `CATALOG_QUERY` returns matching object summaries filtered by
object class, object type, publisher, state, metadata tags, exact metadata
fields, and created/updated time ranges. These filters operate only on object
manifest/catalog metadata; repo nodes still treat payload bytes and signed NDN
Data packets as opaque application data.

The catalog should not periodically broadcast full directories or per-segment
lists. A scalable deployment should publish small periodic deltas, for example
every 10 seconds, and use snapshot/diff recovery when a Persistent Repo joins
late or falls too far behind. Large Data packets remain signed/encrypted NDN
segments; catalog metadata only helps a client decide which repo can serve or
replicate the object.

Deletion is represented as catalog metadata, not as a silent local file
removal. A repo publishes a `DELETED` tombstone entry for the object, and peer
repos must keep that tombstone so older `AVAILABLE` catalog entries cannot
resurrect the object. Conflict handling therefore considers the object's update
time and delete semantics in addition to a peer's local catalog sequence.

Retention is also catalog-level metadata. Objects may carry `ttlMs` and
`repairAllowed` fields. Once an object expires, lookup reports it as `EXPIRED`
and excludes it from repair planning, even if it would otherwise be
under-replicated. Deployments may override object-class defaults in
`repo_control_plane.object_classes`; the repo records these lifecycle fields in
the object manifest while keeping payload bytes opaque:

```yaml
repo_control_plane:
  object_classes:
    uav-recording:
      minReplicationFactor: 2
      maxReplicationFactor: 3
      ttlMs: 604800000
      repairAllowed: true
      autoDelete: false
```

The recommended Python-facing generic object API hides most NDNSF setup
details. In a running deployment, repo nodes can preload the deployment config
as a normal repo object. The application user starts with the repo service
bootstrap parameters and fetches that config through the repo before doing
ordinary `put/get` operations:

```python
from ndnsf_distributed_inference import DistributedRepo

repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
    config_object_name="/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml",
)
manifest = repo.put(
    "APP/Generic/BinaryBlob/demo",
    payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
    metadata={"tags": ["demo", "binary"], "workflow": "example"},
)
payload = repo.get(manifest.object_name, manifest)
objects = repo.list()
matches = repo.catalog_query(
    manifest.replica_nodes[0],
    {"objectClass": "binary-blob", "tags": ["demo"]},
)
repo.remove(manifest.object_name)
```

The high-level `repo.put(...)` API accepts an application-relative suffix and
expands it under the publisher namespace, for example
`/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`.
This keeps app data names globally unique and easy to verify.
Deployment config fetched from the repo follows the same rule; the default
example config object is under the controller publisher namespace.
NDNSF-DistributedRepo treats the supplied payload as opaque application data: whether
the app already encrypted it or left it plaintext, the repo client only
segments and signs the Data packets before storing those segments in repo
nodes.
Validator trust schemas should use hierarchical Data and certificate rules:
stored Data names stay under the publisher identity, child certificates stay
under parent certificate namespaces, and production anchors point at the
trust-root certificate.

## Namespace and Trust Schema Design

NDNSF-DistributedRepo separates the service namespace from the stored-data
namespace. `/NDNSF/DistributedRepo` is only the invocation service name used for
CAPABILITY, STORE, FETCH, MANIFEST, and related operations. Stored objects,
deployment configs, manifests, and payload segments are named under the
application publisher identity.

Recommended object namespace shape:

```text
/<publisher>/NDNSF-DISTRIBUTED-REPO/OBJECT/<app-suffix...>
/<publisher>/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
```

For the generic MiniNDN example:

```text
/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo
/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml
```

The repo does not add an extra encryption layer. Application payloads are
opaque to the repo: encrypted input remains encrypted, plaintext input remains
plaintext, and the repo client only segments and signs the Data packets that
repo nodes store.

Trust roots should be project namespaces, not leaf names. For a project rooted
at `/example/repo`, the trust-root identity is `/example/repo`, and it signs
children such as `/example/repo/controller`, `/example/repo/user`, and
`/example/repo/provider/repoA`. A root named `/example/repo/root` signing
`/example/repo/user` is not hierarchical because `/example/repo/root` is not a
parent prefix of `/example/repo/user`.

The corresponding trust schema should enforce:

- application object Data names are under the publisher identity;
- NDNSF runtime and SVS Data names are under the signer identity;
- certificate names preserve parent-child hierarchy;
- production validation anchors are trust-root certificates, not permissive
  `type any` anchors.

Python Repo storage and service orchestration are retired. Python applications
use the native artifact API for immutable artifact files; generic objects and
the Persistent network service remain C++ APIs. A local artifact store can be
opened as follows:

```python
from pathlib import Path
from py_repoclient import ArtifactReference, ArtifactRepositoryApi

source_path = Path("model.onnx")
destination_path = Path("model.onnx.copy")
artifact_reference = ArtifactReference.from_json(
    Path("model-reference.json").read_text(encoding="utf-8")
)

api = ArtifactRepositoryApi.make_in_app(
    durable_root="/var/lib/ndnsf-artifacts",
    owner_id="/example/repo/user",
)
published = api.publish_file(source_path, artifact_reference)
api.fetch_file(published.reference, destination_path)
```

For authenticated network operations, construct the native Persistent client
from an NDNSF `ServiceUser`:

```python
api = ArtifactRepositoryApi.make_persistent(service_user, client_config)
```

The API borrows that native owner for its lifetime. Run the Face event loop on a
different thread; synchronous API calls release the Python GIL while waiting.
Timeouts bound the wait and do not cancel an outstanding network operation.
Destroy the API before stopping and destroying its `ServiceUser`.

The recommended C++ object API is similarly object-oriented:

```cpp
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"

using namespace ndnsf_distributed_repo;

auto store = makeFilesystemRepoStore("./repo-data");
RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability, store);
StoreOptions options;
options.objectType = "binary-blob";
options.replicationFactor = 2;

auto manifest = RepoClient::put(
  node,
  "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo",
  payload,
  options);
auto fetched = RepoClient::get(node, manifest.objectName);
auto objects = RepoClient::list(node);
RepoClient::remove(node, manifest.objectName);
```

For callers that intentionally store arbitrary opaque bytes as object-level
chunks, the legacy segmented C++ helper stores each chunk as a separate repo
object named `<object>/seg/<N>` and stores a manifest-only parent object.
Callers should still fetch through the object-level
manifest-aware helper; it automatically reassembles segmented objects and
verifies size/hash metadata. The repo stores opaque bytes and does not perform
APP trust, signature, or hash validation while storing; applications verify the
manifest/hash after fetching.

```cpp
auto manifest = RepoClient::putSegmented(
  node,
  "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/model/yolo-stage0",
  payload,
  options,
  6000);
auto verifiedPayload = RepoClient::getObject(node, manifest);
```

`RepoClient::getSegmented(...)` remains available for tests and low-level code
that explicitly needs to exercise the segmented-object path.

## Persistent Repo Node Runtime

The deployed network service is the standalone C++ `DistributedRepoServer`.
Build it and inspect the required identity, authority, trust-schema, and
database arguments:

```bash
./waf build -j4 --targets=DistributedRepoServer
./build/NDNSF-DistributedRepo/DistributedRepoServer --help
```

The server validates its Persistent configuration before creating a Face,
Provider, Repo node, or durable storage files. In-App storage uses a separate native
`ArtifactRepositoryApi` instance and does not start a network service.

## In-App C++ Integration

In-App and Persistent are separate lifecycle paths. Python applications call
`ArtifactRepositoryApi.make_in_app(...)` for local storage and
`ArtifactRepositoryApi.make_persistent(...)` for a caller-owned `ServiceUser`.
Create separate API instances when both paths are needed. No mode string
implicitly copies data between them.

The intended layering is:

```text
RepoCore
  pure storage logic: put/get/manifest/list/delete/capability/catalog

RepoNode
  C++ adapter over RepoCore; exposes trusted local service registration

DistributedRepoServer
  standalone C++ Persistent NDNSF provider and versioned network-service entry
```

`RepoCore` owns object storage, manifest validation, and capacity/catalog
accounting. C++ `RepoNode` registers Persistent network handlers with the
standalone server and exposes trusted local service registration.
`RepoNode::registerLocalServices(LocalServiceRegistry&)` exposes the same
STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE/STATUS/CATALOG_* handlers for
trusted in-process invocation. The in-process repo path still uses the normal
`LocalServiceRegistry::localInvokeRawInto(...)` Request/Response message path; the
important implementation detail is that NDNSF message payloads are carried as
payload TLV `ndn::Block`s internally instead of being passed through a separate
raw byte local API. Applications should still call the typed `RepoClient::local*`
helpers rather than building repo service names or message wrappers by hand.
For a trusted same-process C++ call, register the local service handlers
directly. `RepoDeploymentMode` parser/helper values do not perform registration:

```cpp
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndn-service-framework/LocalServiceRegistry.hpp"

// servicePrefix and capability are configured by the embedding application.
auto store = ndnsf_distributed_repo::makeFilesystemRepoStore("./repo-data");
ndnsf_distributed_repo::RepoNode node(servicePrefix, capability, store);
ndn_service_framework::LocalServiceRegistry localRegistry;
node.registerLocalServices(localRegistry);
```

Embedded callers should use the local `RepoClient` helpers instead of hand
building repo service names or raw `RequestMessage` objects:

```cpp
StoreOptions options;
options.objectType = "model";
options.replicationFactor = 1;
options.replicaNodes = {"/repo/embedded"};

auto manifest = RepoClient::localPut(
  localRegistry,
  servicePrefix,
  "/app/user/NDNSF-DISTRIBUTED-REPO/OBJECT/model/yolo-stage0",
  payload,
  options);
auto payloadAgain = RepoClient::localGet(
  localRegistry,
  servicePrefix,
  manifest.objectName);
```

The C++ `RepoDeploymentMode` parser/helper aliases do not register services and
are not a standalone server configuration interface. Remote callers continue through
the normal NDNSF service path with permissions, signatures, NAC-ABE, and
token/replay protection.

Repo services do not define a separate certificate-selection policy. Remote
repo providers and clients inherit NDNSF runtime behavior: certificate roles are
resolved once at startup, RSA is kept for NAC-ABE/permission unwrap, and
EC/ECDSA is preferred for signing when installed.

Local invocation is only an optimization for trusted same-process composition.
It must not become a wire-protocol mode and must not let remote callers bypass
NDNSF access control.

AI-specific helpers such as `store_artifact(...)` belong in
NDNSF-DistributedInference. NDNSF-DistributedRepo only stores opaque named
objects.

## Application Integration Status

NDNSF-DistributedRepo is intended to support both UAV and distributed inference
workloads, but it should remain a storage and catalog control-plane component
rather than absorb either application's domain logic.

For **NDNSF-UAV-APP**, the repo is the backing layer for recordings, telemetry
logs, mission logs, and other durable data products. The current generic
MiniNDN smoke already stores and looks up representative `uav-recording`,
`telemetry-log`, and `mission-log` objects. The remaining UAV work is
application integration: Ground Station browsing, replay/download UI, and the
mapping from drone/mission identifiers to repo object names.

For **NDNSF-DistributedInference**, the repo is the backing layer for model
artifacts, runtime artifacts, and activation bundles that must outlive a single
service packet. The DI planner/executor should consume a unified
`largeDataReference` abstraction and then choose repo-backed fetch or direct
NDN fetch according to the reference source. Model dependency planning, ONNX
graph analysis, tensor naming, and role scheduling remain DI responsibilities,
not repo responsibilities.

The shared repo responsibilities are deliberately narrower:

- store opaque application objects as segmented, signed Data products;
- publish and merge object-level catalog entries and tombstones;
- report liveness, stale replicas, object class, retention, and repair
  eligibility;
- produce conservative repair plans and optionally execute repair actions when
  deployment policy allows it;
- provide distinct In-App local and Persistent network storage paths without
  changing NDNSF remote invocation semantics.

This boundary keeps the repo useful to both applications without turning it
into a UAV log browser or an AI artifact planner.

## Persistent Storage and Bounded Hot Cache

Repo nodes are initialized with a logical capacity from the C++ Persistent
server configuration. The node advertises
remaining capacity in ACK metadata so clients can choose storage replicas:

```text
repoNode=/example/repo/provider/repoA
freeBytes=...
usedBytes=...
memoryCacheBytes=...
memoryCacheUsedBytes=...
storageBackend=tiered
authoritativeBackend=filesystem
cachePolicy=lru
```

The standalone C++ `DistributedRepoServer` uses a filesystem Repo store for
durable object manifests, opaque payloads, and stored Data packet ranges. Its
store root is derived from the configured database path by appending `.store`.
The separate SQLite database remains authoritative for artifact lifecycle
metadata such as receipts, locations, and operation records. A single
byte-bounded LRU memory tier accelerates Repo reads. Objects and complete
packet sets share the same budget; writes reach the authoritative store before
cache admission, and delete or overwrite changes the cache only after the
authoritative operation succeeds. Oversized entries bypass memory while
remaining available from the filesystem store.

The C++ repo library exposes this policy through `RepoStoreBackend` and
`makeTieredRepoStore(...)`:

```cpp
RepoCore core(capability, makeTieredRepoStore(
  "/var/lib/ndnsf/repo/repo.sqlite3",
  64 * 1024 * 1024));
```

There is no memory-authoritative backend or default memory-only constructor.
The default library factory and tests use SQLite Repo stores or test-local
fault-injection backends; the standalone Persistent server selects the
filesystem Repo store so it can persist range writes. Artifact lifecycle
metadata continues to use SQLite.

The C++ `DistributedRepoServer` exposes the durable stores and bounded memory
cache through `--database`, `--cache-bytes`, and
`--large-object-threshold-bytes`. The database option is a base path from which
the server derives its filesystem Repo store and SQLite artifact metadata
database:

```bash
--database /var/lib/ndnsf-repo/repo-node-A \
--cache-bytes 67108864
```

The server derives `/var/lib/ndnsf-repo/repo-node-A.store` for filesystem Repo
objects and `/var/lib/ndnsf-repo/repo-node-A.artifact-v2.sqlite` for artifact
receipts, locations, and operation records. `--cache-bytes` must be positive;
the server rejects a zero cache budget during configuration validation.

`CACHE_STATUS` is available through the trusted C++ local-service path and the
standalone C++ Persistent network service. It returns
the backend/authority/policy, current
`budgetBytes`/`usedBytes`/`entryCount`, and cumulative `hits`, `misses`,
`admissions`, `evictions`, `invalidations`, `oversizedBypasses`, `backingReads`,
and `backingWrites`. Cache contents and counters are process-local and reset on
restart; filesystem Repo objects and SQLite artifact metadata remain durable.

The Python `py_repoclient` package is a pybind projection of the native C++
`ArtifactRepositoryApi`. Its Persistent factory uses a caller-owned authorized
`ServiceUser` to call the C++ Repo service; Python does not start a separate
Repo server or own a parallel persistent data plane. Generic Repo objects and
packet ranges remain owned by the C++ Repo store, while artifact receipts and
locations remain in the native lifecycle catalog.

The packet API is distinct from the legacy opaque-object chunking helper.
Already-signed Data is stored once in `data_packets` under its exact Data name;
`object_packet_refs` maps logical manifests to those names. Two manifests may
share one immutable packet. Same-name/same-wire insertion is idempotent, while
same-name/different-wire insertion is rejected. `FETCH_PACKET_PREPARE` activates
one known exact name, and packet-backed `FETCH_PREPARE` returns the original
versioned Data name plus `packetNames`. The producer returns the original wire
without re-signing, renaming, re-segmenting, or reconstructing it.

`RepoClient::putSegmented/getSegmented` remains a compatibility helper for
chunking arbitrary opaque bytes under logical `<object>/seg/N` names. It is not
the canonical NDN Data packet path. SegmentFetcher is a consumer-side retrieval
and reassembly helper over the original names; it does not authorize the Repo
to replace those names.

The logical manifest name remains under the authenticated publisher namespace,
but packet names may use an application-owned namespace such as `/data/...`.
The packet API validates every wire name against the declared original Data
prefix; authorization to insert remains an NDNSF service permission decision.

Packet-backed consumers should use the manifest index instead of deriving names:

```cpp
auto wires = RepoClient::getDataPackets(node, manifest);
```

```python
manifest = repo.put_signed_packets(
    object_name, packets,
    object_type="video-segment-set",
    object_size=len(payload),
    object_sha256=sha256)
packets = repo.get_signed_packets(manifest.object_name, manifest)
```

Both operations validate a non-empty, unique `packetNames` index whose length
matches `segmentCount`, fetch each exact complete name in order, decode the wire,
and reject any name mismatch. A replica attempt returns only after the complete
set succeeds; Python retries the whole set at the next declared replica after a
failure. The ordinary `get(...)` operation remains a payload view and may
reassemble content from these same original packets without changing storage.

Replica preparation also returns a short-lived, Repo-specific forwarding hint.
The hint selects the serving Repo while every Interest name remains the original
application Data name. This avoids Repo aliases even when several replicas hold
the same packet prefix. Failover is atomic at packet-set scope: packets from a
failed attempt are discarded, and the next replica starts again at the first
manifest name. The client never combines a partial primary result with a
secondary result.

The two-replica MiniNDN acceptance test stores the same four signed packets at
Repo A and Repo B, terminates Repo A after it returns packet zero, and verifies
that Repo B receives all four names from packet zero onward:

```bash
sudo -n -E python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py \
  --exact-packet-failover-smoke \
  --nlsr-wait-s 10 \
  --repo-start-wait-s 15 \
  --output-dir results/distributed_repo_exact_packet_failover_minindn
```

The result is written to `exact-packet-failover-summary.json`. It records every
Repo/name attempt, exact-name and wire-identity checks, total latency, and
failover latency. The 2026-07-10 acceptance completed in 50,772 ms, including a
42,735 ms failover interval; correctness passed, while the long failed-primary
timeout remains a latency optimization opportunity.

Use this boundary across current applications:

| Caller data | API | Reason |
|---|---|---|
| Already-signed application NDN Data | `put_signed_packets` / `get_signed_packets` | Exact names, signatures, and wires are application identity |
| DI model/runtime files and activation blobs | object `put` / `get` | The caller owns bytes, not encoded Data packets |
| UAV encrypted recording chunks | object `put` / `get` | Encryption output is an opaque byte object |
| Future UAV/DI output already encoded as signed Data | packet API | Preserve the producer's original packet representation |

The dedicated MiniNDN restart/cache acceptance test is:

```bash
sudo -n python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py \
  --tiered-cache-smoke \
  --tiered-cache-bytes 8192 \
  --tiered-cache-object-bytes 4096 \
  --output-dir results/distributed_repo_tiered_cache_minindn
```

It stores three deterministic objects, restarts Repo A on the same database,
and reads `A, A, B, C, A`. The canonical result is written to
`tiered-cache-summary.json`; a passing run must prove a cold backing read, a
repeat hit without another backing read, LRU eviction, SQLite fallback, digest
integrity, and `usedBytes <= budgetBytes`.

## Public large-artifact Python API (`artifact-manifest-v2`)

The C++ `ArtifactRepositoryApi` is the semantic authority. Python's
`ArtifactRepositoryApi` delegates to the same native API and exposes its
reference, handle, receipt, operation, availability, and result types.

```python
from py_repoclient import ArtifactRepositoryApi, ArtifactReference

api = ArtifactRepositoryApi.make_in_app(
    durable_root="/var/lib/ndnsf-artifacts",
    owner_id="/example/application",
)
reference = ArtifactReference.from_json(reference_json)
published = api.publish_file("model.onnx", reference)
api.fetch_file(reference, "cache/model.onnx")
```

The Persistent client factory accepts a caller-owned, authorized
`ServiceUser`. Its `Face` event loop must already run on another thread. The
native wrapper retains the owning `NativeServiceUser`, releases the GIL while
waiting, and must be destroyed before the service user.

The maintained local examples are
`examples/python/NDNSF-DistributedRepo/artifact_api/publish_file.py` and
`examples/python/NDNSF-DistributedRepo/artifact_api/fetch_file.py`. Both call
the native In-App implementation. Persistent server behavior is provided by
the standalone C++ `DistributedRepoServer`.

The Python wrapper supports synchronous native operations: publish by file and
complete reference, begin/status publication, candidate discovery, explicit
archive, fetch by reference or handle, local commit lookup, receipt lookup, and
availability checks. Python upload/fetch sessions, async wrappers, progress or
cancellation callbacks, backend injection, and Python placement logic are
retired. Their import-compatible names raise `RepoCompatibilityError` with
code `SPEC192_COMPATIBILITY_V1` and a native replacement path.

`ArtifactPublishResult` and `ArtifactFetchResult` are native result values.
Native fetch failures retain the structured `ArtifactFetchResult` on
`ArtifactApiError`; validation errors remain native argument/validation
exceptions. No Python code simulates Repo behavior.

### Artifact schema migration and rollback

Artifact metadata uses additive native SQLite migrations. The current schema
and supported prior generations are defined by the C++ data model and migration
selectors; the Python wrapper does not open or upgrade Repo databases. The
offline legacy importer reads its source without mutation and writes only to a
new C++ destination.

The standalone C++ server fails closed when its artifact catalog is unavailable
or newer than the runtime supports. It does not rewrite committed payload bytes
or legacy exact-packet wires during startup.

ACK and `CAPABILITY` payloads expose bounded `artifactMigration` diagnostics.
A read-only node withdraws `artifact-manifest-v2`, resume, and durable-receipt
support from its advertisement, so capability negotiation rejects it before
transfer. `exact-packet-v1` remains separately advertised and its trust
semantics are unchanged.

## Python Binding

From the checkout root, install the native stack and Python wheels with:

```bash
./install_ndnsf_stack.sh
```

Waf installs the C++ targets, public headers and Repo archive; it does not run
pip. The installer builds the binding wheel from its two binding files and
links the same Waf archive. Standalone wheel builds require the matching
installed Core pkg-config metadata, Repo archive and native-library digest
receipt; a source checkout does not replace that SDK.

The `py_repoclient` package projects C++ artifact configs, values, results, and
methods. It converts `Path` arguments and maps native fetch errors. It does not
own a filesystem/network backend, Repo server handler, receipt validator,
placement state machine, operation journal, or retry policy. Legacy Python-only
APIs raise `RepoCompatibilityError` with the code
`SPEC192_COMPATIBILITY_V1` and a replacement path.

The standalone server remains usable without Python. Higher-level applications
may prepare application data and reference metadata; C++ owns Repo storage,
network handlers, receipts, current availability, placement/consistency, and
fetch verification.

## Persistent C++ runtime contract

For the standalone Persistent server, the filesystem Repo store owns durable
object manifests and payload or packet ranges, while SQLite owns artifact
receipts, locations, and operation records. The bounded memory cache accelerates
Repo reads. Writes return native durable receipts, and `ONE`, `QUORUM`, and
`ALL` determine the required selected-target count. ACK metadata is advisory;
the selected Repo performs definitive capacity checks before committing.
Persistent handlers are registered by `DistributedRepoServer` using the
configured Provider identity.

The native Repo preserves exact app-signed Data wire bytes. Artifact-v2 values,
receipts, status, location, and availability are validated by the C++ API and
production service. Python wrapper conformance does not replace those C++ unit,
integration, or MiniNDN gates.

## Targeted parallel control plane

When the replica set is already known, Repo control operations use NDNSF
Targeted invocation after the normal authenticated token bootstrap. Replicated
store assignments are submitted asynchronously through one `ServiceUser` and
share one total deadline; there is no preceding reserve/release round trip.
Successful sibling receipts are retained when another replica fails, and the
final write still has to satisfy the requested `ONE`, `QUORUM`, or `ALL`
consistency level.

Targeted invocation is an optimization, not a security bypass. Permission
checks, NAC-ABE protection, one-time provider tokens, replay protection,
operation IDs, receipt validation, and write-consistency checks remain active.
An optional bounded fallback keeps older Normal-only providers usable and is
reported separately in the control metrics.

### Historical Python client campaign measurements

The Python `NetworkDistributedRepoClient` and its `control_metrics()` API were
retired by Spec 192 T008. The measurements below describe that earlier
implementation only. They are not current API conformance or T010 qualification
evidence; current service behavior is owned by the C++ API and its production
gates.

Matched 60-second MiniNDN campaigns with RF=2 and W=ALL produced:

| Workload | Normal write p95 | Targeted write p95 | Targeted completion |
|---|---:|---:|---:|
| c16, 2 RPS, 90% reads | 39,838.543 ms | 243.134 ms | 120/120 |
| c4, 0.5 RPS, 10% reads | 10,583.983 ms | 192.855 ms | 30/30 |

The first parallel RF=2 run also exposed unsafe cross-thread use in the
OpenABE/RELIC backend. NAC-ABE now serializes OpenABE work on one dedicated
process-wide worker initialized on that same thread. This preserves correctness
while making the backend's per-process ABE serialization boundary explicit.

See `specs/078-repo-targeted-control-plane/quickstart.md` for exact commands and
`specs/078-repo-targeted-control-plane/results.md` for accepted evidence.

### RF=3 quorum during provider loss

Desired replication and write acknowledgement thresholds are separate. An
RF=3/W=QUORUM write may commit with two validated durable receipts when one
desired Repo is unavailable; its manifest retains `replicationFactor=3` for
later repair and lists only receipt owners in `confirmedReplicaNodes`. W=ALL
still requires all three receipts.

The Selection assignment names the desired replicas, while the receipt
threshold decides whether the result commits. No capacity reservation is
created between those steps. Targeted and fallback outcomes update provider
health; a provider that fails both paths enters a stronger cooldown than a
transient Targeted failure whose Normal fallback succeeds.

In the matched 60-second RF=3/W=QUORUM MiniNDN run, RepoA was stopped after 20
seconds. All 19 post-failure requests succeeded, including 17 writes with
exactly two receipts; post-failure write p50/p95 was 178.457/1,649.912 ms. One
pre-failure write failed because all three Targeted deliveries timed out before
the injected failure, so the overall result was 29/30 rather than being
misreported as provider-loss failure. See
`specs/079-repo-targeted-quorum-failure/results.md`.

### Online repair after the provider returns

The failed Repo and its catalog sidecar are one recovery unit. When the Repo
process restarts, the sidecar restarts with the same identity, peers, policy,
and persistent storage. It publishes fresh membership, merges peer catalog
deltas, scans durable repair jobs, and copies missing objects through the
existing validated NDNSF repair service path. The campaign harness observes
this process; it does not copy SQLite files or payloads directly.

In the 60-second RF=3/W=QUORUM recovery run, all 30 requests completed. Five
writes completed while RepoA was strictly offline. After RepoA restarted 12
seconds later, its sidecar created ten durable jobs and completed three repairs;
one belonged to the strict outage set, giving 20% bounded-window coverage. The
first repaired object completed 15.015 seconds after restart. RepoA persistence
and matching AVAILABLE catalog entries from A/B/C with the same digest verified
that object at RF=3. The other four outage objects remained visible as repair
backlog rather than being reported as restored. See
`specs/080-repo-online-repair-recovery/results.md`.

### Bounded repair workers and quorum finalization

Repair jobs now carry durable risk, priority, age, and missing-replica fields.
Claims prefer objects with fewer available replicas, then higher priority and
older objects. The sidecar may run 1--8 independent object transfers, but
scan, claim, complete, and fail remain serialized through one `ServiceUser`;
the production default remains one worker.

A local multi-replica write is now `STAGED` until the user validates W durable
receipts and sends protected `FINALIZE_WRITE` requests. Staged generations are
excluded from inventory, reads, Data-plane activation, repair sources, and
repair jobs. A write below quorum therefore cannot be resurrected by repair.

Matched 60-second MiniNDN runs completed 30/30 requests with a receipt floor of
two and zero repair events for failed writes. Workers=1 repaired 2/4 strict
outage objects (50%) with request p50/p95 239.371/5,371.381 ms. Workers=3
repaired 1/4 (25%) with 318.392/5,660.957 ms. The pool is correct and
configurable, but this campaign showed no throughput benefit because
catalog/control-path job visibility, not transfer capacity, was limiting.
See `specs/081-repo-bounded-parallel-repair/results.md`.

### Repair fast path and phase observability

Durable repair no longer probes a catalog-known missing target with
`FETCH_PREPARE`. That negative ACK previously became a fixed selection timeout
on the single client owner thread and serialized worker startup. Repair now
starts from source `FETCH_PREPARE` and still requires exact Data retrieval,
packet/object hashes, repair authorization, target persistence, lease, and
completion checks.

`REPAIR_SCAN` reports durable state and target-local claimability. Sidecar logs
record peer merge batches/duration and repair cycle scan, claim, transfer, and
total duration. The MiniNDN summary parses these metrics for direct bottleneck
attribution.

In the single planned matched workers=3 campaign, requests remained 30/30 with
W=2 and zero invalid repairs. Strict outage coverage improved from 1/4 to 4/4,
first repair from 20.248 to 10.587 seconds, and request p95 from 5,660.957 to
1,814.117 ms. The initial cycle exposed nine claimable jobs and completed six
in 0.838 seconds. Catalog merge batching is now the next measured bottleneck.
See `specs/082-repo-repair-fast-path-observability/results.md`.

### Large catalog merge over exact segmented Data

Large anti-entropy deltas now use one protected `CATALOG_MERGE_PULL` control
request instead of many serial inline merge batches. The source publishes an
immutable, signed segmented object; the request carries its exact name,
schema version, byte and entry counts, and SHA-256 digest. The target retrieves
the complete object through SegmentFetcher and validates every bound field
before applying the catalog entries. Payloads up to 6,000 bytes remain inline,
the pull path is capped at 16 MiB, and a failed pull falls back to the previous
bounded batch path.

The matched workers=3 MiniNDN treatment preserved 30/30 requests, W=2, zero
invalid repairs, and 4/4 outage-object recovery. The recovered sidecar used six
pull merges and two inline merges with no fallback. Its two initial 37/39-entry
deltas each required one control request rather than 16 batches. Aggregate
merge time fell from 5,200.463 to 3,038.567 ms; first repair after restart
improved from 10.587 to 9.033 seconds, while request p95 remained similar at
1,779.222 ms. See `specs/083-repo-catalog-merge-large-data/results.md`.

## Spec 164 Scalable Artifact Path and Current Acceptance Boundary

The scalable path is implemented as `artifact-manifest-v2`. Its ownership
boundary is:

```text
application / NDNSF-DI
  -> ArtifactRepositoryApi
  -> ordinary NDNSF Collaboration control
  -> selected Repo providers
  -> segmented NDN Data transfer
  -> authoritative payload and metadata stores
```

NDNSF owns Request/ACK/ACK_CLOSED/Selection authorization, NAC-ABE routing,
one-time tokens, replay protection, and the Collaboration lifecycle. The
repository owns advisory store offers, bounded task queues, artifact capability
negotiation, manifests, segmented transfer, replica receipts, atomic
visibility, resume, recovery, and garbage collection.
NDNSF-DI supplies model-specific artifact identities and consumes the public
repository API; model splitting and inference scheduling do not belong in the
repository.

The default store-control lifecycle deliberately has no ACK-time reservation
or resource lock:

```text
Request
  -> advisory ACK offers
  -> ACK_CLOSED
  -> commit_plan with exact store assignments
  -> Provider bounded task queue
  -> QUEUED -> RECEIVING -> VERIFIED -> COMMITTED -> ACTIVE
  -> Response
```

A positive ACK reports a bounded snapshot such as queue depth/capacity and
available/max bytes. It says that the Provider is willing and appears able to
accept the task; it does not promise that those bytes remain reserved. The
selected Provider enqueues the exact assignment and performs execution-time
admission when the task reaches the worker. Capacity drift produces an
explicit task failure, not an ACK-time lock. Legacy upload-lease collaboration
types remain readable for compatibility and migration only; new publication
uses `ReplicaTaskCollaborationClient`.

The public application surface is synchronous, asynchronous, and resumable:

```python
published = repo.publish_file(
    source,
    name="/models/qwen/stage-0",
    expected_sha256=digest,
    replicas=3,
)

fetched = repo.fetch_file(
    published.reference,
    destination,
    verify=True,
)

upload = repo.begin_upload(descriptor)
upload.upload_file(source)
published = upload.commit()
```

`ArtifactReference` is the immutable reuse identity: logical name, full
content digest, byte size, signed root-manifest name, publisher identity, and
policy epoch. A logical name alone is insufficient. The default deployed
control mode is ordinary Collaboration; Targeted is an explicit known-provider
optimization and does not bypass authorization, tokens, manifest validation,
or durable receipt checks.

The trust composition deliberately avoids one public-key verification per Data
packet. One publisher-signed root binds the artifact identity and the
content-addressed manifest hierarchy. Data packets and chunks are checked with
bounded digest work, the complete payload digest is checked before activation,
and each committed replica returns an authenticated receipt. HMAC receipts are
efficient evidence inside one NDNSF authorization domain; they are not public
non-repudiation across mutually distrustful administrative domains.

Payload bytes are streamed to content-addressed files. Metadata, journals,
receipts, and catalog state remain transactional. Temporary and verified data
stay hidden until durable metadata commit and activation. Restart
reconciliation, exact-identity resume, generation fencing, execution-time
capacity validation, exclusive object ownership, and GC ownership rules
prevent partial content, stale retries, or unowned temporary files from
becoming visible. These short metadata/object critical sections are not
resource reservations advertised by ACK. `exact-packet-v1` remains an explicit
compatibility format and is never silently reinterpreted as v2. Future or
operator-disabled schema generations become read-only rather than rewriting
existing bytes.

### Frozen MiniNDN results

The active canonical confirmatory campaign is:

```text
results/spec164-artifact-confirmatory-campaign-20260730T1030Z
campaignId: spec164-artifact-20260730T101414Z
manifestSha256: 0fbcd3d4f77dbadba5e90eaad3a23d8425f9a67e0fbd9fca401cc38aff568996
```

It admitted 24 of 96 preflight cells and retained 24/24 passing warmups and
120/120 passing measured runs, with exactly five measured repetitions per
cell. The predeclared large-artifact domain is 64 MiB and above; every 1 MiB
diagnostic also remains present and passed.

The determinate gates are:

- SC-002 PASS: 64 MiB digest/raw median `0.975401`, bootstrap interval
  `[0.925489, 1.015141]`, and complete measured runs.
- SC-003 PASS: 64 MiB signed/digest median `0.996644`, bootstrap interval
  `[0.984451, 1.069018]`; completion, point-estimate, and lower-bound gates
  all pass.
- SC-004 PASS: a separate public Collaboration smoke observed 2 control
  operations for 16 publication Data segments.
- SC-007 PASS: publication and fresh-destination cold retrieval satisfy
  separate payload/metadata read/write amplification bounds.

Reproduce the immutable analysis:

```bash
python3 Experiments/analyze_distributed_repo_artifact.py \
  --campaign results/spec164-artifact-confirmatory-campaign-20260730T1030Z \
  --output-json \
    results/spec164-artifact-confirmatory-campaign-20260730T1030Z/derived-results.json \
  --output-markdown \
    specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t031-performance-report.md \
  --control-evidence \
    results/spec164-public-task-minindn-20260730T0734Z/summary.json
```

All predecessor campaigns remain immutable, including the third campaign's
three first-Interest failures and original SC-003 FAIL. The confirmatory run
did not repeat the predecessor's physical ceiling, so 659.613 Mbit/s remains
historical environment evidence.

Spec 164 now permits TigerCluster NDNSF-DI + Qwen external-validity work. That
work must still use immutable `ArtifactReference` values, ordinary NDNSF
Collaboration assignments, repository publication/fetch, and durable evidence
bindings, and must pass Specs 162/163's own gates. MiniNDN results are not
themselves TigerCluster or large-model results.

### TigerCluster Qwen3.6 operational lessons (2026-08-01)

The first explicitly authorized Spec 162 run exercised the real
NDNSF-DI/DistributedRepo publication path on three RTX 5000 nodes. The three
content-addressed Qwen3.6-27B stages total 53,792,308,358 bytes and are bound
to manifest
`sha256:cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787`.
Jobs 181527, 181528, 181530, 181531, and 181532 are preserved failed
identities with pre-generation causes. Job 181532 rejected the first Selection
because the synthetic Provider boot epoch differed from Core's native epoch.
None is a complete-generation `PASS` by itself.

The cold path must be instrumented as four phases: (1) NDN chunk publication,
(2) root-manifest registration and catalog `ACTIVE` commit, (3) Provider
verification/fetch/GPU preparation and cache state, and (4) secured DI
execution. Staged files are not an active catalog. In the observed 53.79 GB
runs, registration became visible only after roughly 19–22 minutes of job
time. This is diagnostic end-to-end evidence, not a repository-throughput
measurement; Spec 167 owns controlled payload and matched-network goodput
claims.

Deployment must also bind the campaign path to `/shared/<basename>`, resolve
the tokenizer from the bound artifact directory, and seal `user.py` together
with its matching `llm_pipeline_lib.py`. A missing native SIF symbol is a
runtime-closure failure, not a repository or model-correctness result. The
live-003 provider logs also emitted repeated `ProviderToken mismatch` messages
during Repo STORE control selections; publication completion does not erase
that authorization/correlation diagnostic, which must remain visible to the
acceptance analyzer. The
complete evidence and failure table are maintained in
`docs/NDNSFDI/tigercluster-qwen36-operational-lessons.md`.

The linked live-006 and live-007 diagnostic identities are preserved: live-006
showed that terminal-selection fail-fast must not be enabled for generic Repo
STORE collaborations, while live-007 exposed a missing sealed-runtime
`DISelectionAssignmentV2` import. The Repo ACK path now sets a bounded,
size-aware `pending_state_ttl_ms` (conservative 8 MiB/s estimate plus five
minutes, capped at one hour) so Core does not expire the consumed
ProviderToken while a large artifact is pulled, verified, and finalized. This
protects request-state lifetime only; Repo still queues work without a storage
lock or reservation. The fix-009 source manifest is
`ec8212dbe219f492d0e4474837bdb0208fdf2a079ab42c2409a0c0393e65850d`.

Follow-up identities remain separate evidence: live-009 (181538) found that
`AckDecision` is frozen and must receive `pending_state_ttl_ms` in its
constructor; live-011 (181539) proved corrected Repo ACK and DI assignment
coverage but required binding the current DI provider module; live-012 (181541)
exposed invalid `DIRoleAssignmentV2.artifact` cache-key access; live-013
(181543) then exposed duplicate DI GPU admission on Selection. Fix-014 reuses
the existing unexpired offer for the same request/attempt/model/roles/backends
binding; live-014 (181544) is the resulting requalification. These are
source/runtime closure and preparation fixes, not Repo throughput claims.
