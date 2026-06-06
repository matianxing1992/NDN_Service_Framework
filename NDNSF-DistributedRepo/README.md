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
Data packets.

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

For large objects, the preferred NDN-native path is for the application to
publish signed and optionally encrypted segmented Data under its own namespace.
The repo request then carries only a `RepoDataReference`: object name, Data
prefix, optional segment range/final segment hint, forwarding hint, expected
size, and expected SHA-256. The repo fetches those Data packets through an
injected SegmentFetcher adapter, stores each fetched wire packet as opaque
bytes under `<objectName>/ndn-data/<N>`, and stores a manifest-only parent
object.

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

For larger payloads, store with the segmented C++ helper. It stores each chunk
as a separate repo object named `<object>/seg/<N>` and stores a manifest-only
parent object. Callers should still fetch through the object-level
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

## Storage Backend

Repo nodes are initialized with a logical capacity, for example the
`free_bytes` parameter in the Python `RepoNodeApp`. The node advertises
remaining capacity in ACK metadata so clients can choose storage replicas:

```text
repoNode=/example/repo/provider/repoA
freeBytes=...
usedBytes=...
memoryCacheBytes=...
memoryCacheUsedBytes=...
storageBackend=sqlite
```

The current persistent backend is SQLite. Each row stores the object manifest
and payload bytes, and the `payload_size` column is used to compute remaining
capacity. Each repo node also maintains an in-memory LRU cache for recently
stored or recently fetched objects. The cache is an optimization only; SQLite
is the source of truth.

The C++ repo library exposes the same choice through `RepoStoreBackend`.
`RepoCore` defaults to an in-memory store for tests and embedded ephemeral use,
while `makeSqliteRepoStore(path)` gives a persistent backend:

```cpp
RepoCore core(capability, makeSqliteRepoStore("/var/lib/ndnsf/repo/repo.sqlite3"));
```

`DistributedRepoNodeApp` reads this from config:

```text
storage-backend sqlite
storage-path /tmp/ndnsf-distributed-repo/repo-node-A.sqlite3
```

Objects written through the SQLite backend remain fetchable after the repo app
or embedding process restarts.

The object API treats stored bytes as opaque application objects or opaque
segment records behind a manifest. Applications should not depend on SQLite row
layout or cache internals. When an application already publishes signed
segmented Data, the repo-facing reference path stores the fetched segment
records as opaque bytes and reports their manifest/catalog metadata. Serving
raw Data wire packets directly on matching Interests can be optimized below the
object API, but it should not change the public `put/get/insert/fetch` object
semantics.

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
