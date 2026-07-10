# NDNSF-DistributedRepo

NDNSF-DistributedRepo is an experimental C++ subproject for a distributed,
NDN-native, policy-controlled artifact and intermediate-data storage layer for
NDNSF applications.

The current implementation is intentionally small, but it is already organized
as both a reusable C++ API and an NDNSF service layer:

- `RepoObjectManifest`: signed-manifest-ready metadata for one stored object.
- `StorageCapability`: advertised capacity/load/mode information for a repo
  node.
- `RepoCatalogStatus`, `RepoCatalogEntry`, and `RepoCatalogDelta`: logical
  object-level catalog metadata for replica discovery and recovery.
- `PlacementPolicy`: controlled replication requirements.
- `selectReplicas`: deterministic placement over candidate storage nodes.
- `InMemoryRepoStore`: a local smoke-test store used by examples.
- `RepoCore`: reusable in-process storage logic with no dependency on NDNSF
  networking.
- `RepoNode`: a repo node that can register NDNSF services on a
  `ServiceProvider` and delegates storage operations to `RepoCore`.
- `RepoClient`: high-level `put/get/list/remove` helpers, plus lower-level
  request helpers for applications that need direct `ServiceUser` control.
- `py_repoclient`: a Python binding for the reusable repo client/protocol API.

The service name is application-configurable. By default, all repo nodes share:

```text
/NDNSF/DistributedRepo
```

The C++ repo node exposes operation-specific services under this prefix. The
repo-level operation is `INSERT`: an application publishes signed and optionally
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

The code keeps the old `embedded` mode string for compatibility, but the design
term is In-App Repo. The accepted mode strings are:

```text
remote          standalone Persistent Repo service path
embedded        compatibility spelling for In-App Repo
in-app          preferred spelling for In-App Repo
both            expose both remote and in-process paths
```

`StorageCapability` now carries `repoMode` and `acceptsBackupReplica`. Placement
logic filters out candidates with `acceptsBackupReplica=false` when creating
backup replicas. In-App Repos may still advertise cached objects, but their
catalog role should be treated as local/ephemeral unless the application
explicitly marks them as durable.

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

`DistributedRepo.from_config("repo_policy.yaml")` remains available for local
tests and deployment tools that already have the policy file on disk.

The lower-level Python binding remains available when a framework already owns
the NDNSF `ServiceUser`:

```python
from py_repoclient import RepoClient

repo = RepoClient(user, "/NDNSF/DistributedRepo")
manifest = repo.insert(
    object_name="/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo",
    payload=payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
)
payload = repo.fetch(manifest.object_name)
```

If the caller already has a manifest and wants one logical object, prefer the
manifest-aware helper:

```python
payload = repo.fetch_object(manifest)
```

It verifies the returned payload against the manifest size and hash. This keeps
application code independent from the repo's internal single-payload versus
segmented-object layout.

The recommended C++ object API is similarly object-oriented:

```cpp
using namespace ndnsf_distributed_repo;

RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability);
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

## C++ Standalone Repo Node

`DistributedRepoNodeApp` runs the same `RepoNode` adapter as a standalone NDNSF
provider. It is configured with a small key/value file:

```bash
./build/NDNSF-DistributedRepo/DistributedRepoNodeApp \
  --config NDNSF-DistributedRepo/configs/repo-node.conf
```

The default example config sets:

```text
service-prefix /NDNSF/DistributedRepo
identity /example/repo/repo/A
controller-prefix /example/repo/controller
repo-node /example/repo/repo/A
deployment-mode remote
repo-mode persistent
accepts-backup-replica true
storage-backend tiered
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
memory-cache-bytes 67108864
```

`deployment-mode remote` is the normal standalone Persistent Repo mode. The app
also accepts `embedded`/`in-app` and `both` so the same config vocabulary can be
shared with service containers, but local invocation is only useful to code
running in the same trusted process.

For build and config validation without starting NFD or a controller:

```bash
./build/NDNSF-DistributedRepo/DistributedRepoNodeApp \
  --config NDNSF-DistributedRepo/configs/repo-node.conf \
  --deployment-mode embedded \
  --dry-run \
  --local-smoke
```

## In-App Repo Plan

NDNSF-DistributedRepo is being organized so the same repo implementation can run
either as a standalone repo application or as an embedded service inside another
trusted NDNSF service container.

The intended layering is:

```text
RepoCore
  pure storage logic: put/get/manifest/list/delete/capability/catalog

RepoNode
  NDNSF service adapter: registers STORE/FETCH/MANIFEST/INVENTORY/
  CAPABILITY/DELETE/STATUS/CATALOG_* on a ServiceProvider and delegates every
  operation to RepoCore

Repo app or embedding container
  chooses whether to expose RepoNode remotely, register local services, or both
```

Current first step: `RepoCore` owns the object store, manifest validation, and
capacity/catalog accounting. `RepoNode` keeps the existing public API and
standalone service registration behavior, but now delegates storage and catalog
work to `RepoCore`.
`RepoNode::registerLocalServices(LocalServiceRegistry&)` exposes the same
STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE/STATUS/CATALOG_* handlers for
trusted in-process invocation. The in-process repo path still uses the normal
`LocalServiceRegistry::localInvokeRawInto(...)` Request/Response message path; the
important implementation detail is that NDNSF message payloads are carried as
payload TLV `ndn::Block`s internally instead of being passed through a separate
raw byte local API. Applications should still call the typed `RepoClient::local*`
helpers rather than building repo service names or message wrappers by hand.
`RepoDeploymentMode` selects which exposure path to enable:

```cpp
RepoNode node(servicePrefix, capability);
LocalServiceRegistry localRegistry;

node.registerDeploymentServices(
  nullptr,
  &localRegistry,
  RepoDeploymentMode::Embedded);
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

Supported mode strings are `remote`, `embedded`, `in-app`, and `both`. Empty
config keeps the default `remote` behavior for existing standalone repo
deployments.

Next steps:

1. Let higher-level applications such as NDNSF-UAV-APP and
   NDNSF-DistributedInference load this deployment mode from their service
   container config and wire the embedded registry into their own components.
2. Keep remote callers on the normal NDNSF service path with permissions,
   signatures, NAC-ABE, and token/replay protection.

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
- expose In-App and Persistent deployment modes without changing NDNSF remote
  invocation semantics.

This boundary keeps the repo useful to both applications without turning it
into a UAV log browser or an AI artifact planner.

## SQLite Authority and Bounded Hot Cache

Repo nodes are initialized with a logical capacity, for example the
`free_bytes` parameter in the Python `RepoNodeApp`. The node advertises
remaining capacity in ACK metadata so clients can choose storage replicas:

```text
repoNode=/example/repo/provider/repoA
freeBytes=...
usedBytes=...
memoryCacheBytes=...
memoryCacheUsedBytes=...
storageBackend=tiered
authoritativeBackend=sqlite
cachePolicy=lru
```

Every deployed Repo node uses SQLite as its sole authority for manifests,
opaque payloads, and stored Data packets. A single byte-bounded LRU memory tier
accelerates newly committed and repeatedly fetched entries. Objects and complete
packet sets share the same budget. Writes commit SQLite before cache admission;
reads consult memory and fall back to SQLite; delete and overwrite change the
cache only after the authoritative transaction succeeds. An oversized entry
bypasses memory but remains available from SQLite.

The C++ repo library exposes this policy through `RepoStoreBackend` and
`makeTieredRepoStore(...)`:

```cpp
RepoCore core(capability, makeTieredRepoStore(
  "/var/lib/ndnsf/repo/repo.sqlite3",
  64 * 1024 * 1024));
```

The in-memory C++ backend remains an internal test double for direct unit tests;
it is not a supported Repo-node deployment mode. The standalone app rejects
`storage-backend memory` instead of silently losing persistence.

`DistributedRepoNodeApp` reads this from config:

```text
storage-backend tiered
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
memory-cache-bytes 67108864
```

Set `memory-cache-bytes 0` to disable hot-cache admission while retaining the
same SQLite authority and object API. This is SQLite-only operation, not a
memory-only compatibility mode.

`CACHE_STATUS` is available through direct, embedded, remote NDNSF, and Python
client paths. It returns the backend/authority/policy, current
`budgetBytes`/`usedBytes`/`entryCount`, and cumulative `hits`, `misses`,
`admissions`, `evictions`, `invalidations`, `oversizedBypasses`, `backingReads`,
and `backingWrites`. Cache contents and counters are process-local and reset on
restart; SQLite objects do not.

The persistent Python Repo follows the same ordering and derives a deterministic
SQLite path under `/tmp/ndnsf-distributed-repo/` when no storage directory is
given. It does not duplicate all persistent payloads in an unbounded Python
dictionary. After a restart, a network fetch first sends `FETCH_PREPARE` to the
selected Repo. The Repo loads the complete object or packet set through the hot
cache/SQLite path, starts a bounded-lifetime Data producer, and then the client
uses the normal segmented NDN fetch path. Temporary producers are serving
resources, not authoritative storage or hot-cache entries.

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

## Python Binding

`NDNSF-DistributedRepo/pythonWrapper` installs an importable package named
`py_repoclient`. It exposes the same generic concepts as the C++ API:

```python
from py_repoclient import (
    RepoClient,
    RepoObjectManifest,
    StorageCapability,
    PlacementPolicy,
    select_replicas,
)
```

The binding is installed automatically during `./waf install` through
`python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper`. For source-tree
development it can also be installed manually:

```bash
python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper
```

Higher-level frameworks such as NDNSF-DistributedInference should use
`py_repoclient` for repo service operations and placement metadata, while
keeping AI-specific helpers such as `store_artifact(...)` in their own package.

The long-term service design is:

```text
NDNSF-DistributedInference
  decides model roles, dependency graph, and runtime needs

NDNSF-DistributedRepo
  stores model/runtime/intermediate objects with controlled replication

NDNSF Core
  provides service discovery, selection, signing, NAC-ABE, and SVS transport
```

This subproject is not a replacement for NDN repo-ng. It is a policy-controlled
NDNSF storage layer: NDNSF applications can ask a repo cluster where an object
should live, store it with bounded replication, and fetch it later through NDNSF
service invocation and normal NDNSF authentication/authorization paths.
# High-availability runtime contract

The persistent Repo path uses SQLite as authority and a bounded memory LRU as
acceleration. Writes carry an idempotent operation ID and return durable
per-replica receipts; `ONE`, `QUORUM`, and `ALL` determine the required receipt
count. Capacity reservations prevent concurrent writers from oversubscribing a
node. Placement cache entries expire, and overload, timeout, capacity, or
integrity failures invalidate the affected node and place it in a short health
cooldown.

Repo Data is served by one long-lived producer per process. Exact app-signed
Data wires are stored and returned under their original full names without
renaming or re-signing. Opaque large objects continue to use SegmentFetcher;
continuous publication is a stream concern and is not part of the Repo object
API.

Catalog journal entries, tombstones, peer watermarks, membership heartbeats,
repair jobs, and capacity reservations survive restart. Bucket digests support
bounded anti-entropy. Same-generation live entries with different content
digests are reported as `CONFLICT`; the repair scheduler does not choose one
silently. Repair jobs are idempotent, leased, retried with backoff, and scanned
periodically instead of being suppressed by process-lifetime sidecar state.

The C++ library is the canonical object/protocol contract. Python owns NDNSF
network orchestration, SQLite catalog/repair persistence, placement telemetry,
and MiniNDN campaigns. `repo-ng` command-wire compatibility is a future adapter,
not a second internal policy implementation.

## Targeted parallel control plane

When the replica set is already known, Repo control operations use NDNSF
Targeted invocation after the normal authenticated token bootstrap. Capacity
reservation, reservation release, and replicated store calls are submitted
asynchronously through one `ServiceUser` and share one total deadline.
Successful sibling receipts are retained when another replica fails, and the
final write still has to satisfy the requested `ONE`, `QUORUM`, or `ALL`
consistency level.

Targeted invocation is an optimization, not a security bypass. Permission
checks, NAC-ABE protection, one-time provider tokens, replay protection,
operation IDs, receipt validation, and write-consistency checks remain active.
An optional bounded fallback keeps older Normal-only providers usable and is
reported separately in the control metrics.

`NetworkDistributedRepoClient` accepts `control_mode="normal"` or
`control_mode="targeted"` and exposes `control_metrics()`. Campaign lifecycle
CSV files include `reserveMs` and `storeMs`; summary JSON records Targeted,
normal, timeout, fallback, fan-out, and maximum-concurrency counters. The
Targeted token batch can be tuned with `NDNSF_TARGETED_TOKEN_BATCH_SIZE`
(1--256, default 8).

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

Capacity reservation follows the same threshold. When reservation is enabled,
store requests are sent only to providers that returned valid reservations.
Targeted and fallback outcomes update provider health; a provider that fails
both paths enters a stronger cooldown than a transient Targeted failure whose
Normal fallback succeeds.

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
