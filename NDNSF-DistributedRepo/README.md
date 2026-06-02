# NDNSF-DistributedRepo

NDNSF-DistributedRepo is an experimental C++ subproject for a distributed,
NDN-native, policy-controlled artifact and intermediate-data storage layer for
NDNSF applications.

The current implementation is intentionally small, but it is already organized
as both a reusable C++ API and an NDNSF service layer:

- `RepoObjectManifest`: signed-manifest-ready metadata for one stored object.
- `StorageCapability`: advertised capacity/load information for a repo node.
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

The C++ repo node exposes operation-specific services under this prefix, for
example `/NDNSF/DistributedRepo/STORE` and `/NDNSF/DistributedRepo/FETCH`.
Current C++ operations include:

```text
CAPABILITY
STORE
STORE_MANIFEST
FETCH
MANIFEST
INVENTORY
DELETE
```

Python/NDNSF-DI helpers may also use higher-level operations such as
`STORE_FROM_NDN` and `FETCH_PREPARE` for app-owned segmented Data packets.

Objects are described by `RepoObjectManifest`, which contains object name,
object type, SHA-256, size, segment count, replication factor, selected replica
nodes, and policy epoch. Large object transfer can use app-owned ndn-cxx
Segmenter/SegmentFetcher Data packets, or the C++ `RepoClient` object-level
segmented helpers. Placement and replica selection use NDNSF service discovery
and ACK metadata. This keeps the repo generic: the payload may be a model
shard, runner, ONNX file, PyTorch artifact, activation tensor,
payment-workflow record, telemetry log, JSON configuration, or any other NDNSF
application object.

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
)
payload = repo.get(manifest.object_name, manifest)
objects = repo.list()
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
manifest = repo.store(
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
```

`deployment-mode remote` is the normal standalone mode. The app also accepts
`embedded` and `both` so the same config vocabulary can be shared with service
containers, but local invocation is only useful to code running in the same
trusted process.

For build and config validation without starting NFD or a controller:

```bash
./build/NDNSF-DistributedRepo/DistributedRepoNodeApp \
  --config NDNSF-DistributedRepo/configs/repo-node.conf \
  --deployment-mode embedded \
  --dry-run \
  --local-smoke
```

## Embedded Repo Plan

NDNSF-DistributedRepo is being organized so the same repo implementation can run
either as a standalone repo application or as an embedded service inside another
trusted NDNSF service container.

The intended layering is:

```text
RepoCore
  pure storage logic: put/get/manifest/list/delete/capability

RepoNode
  NDNSF service adapter: registers STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE
  on a ServiceProvider and delegates every operation to RepoCore

Repo app or embedding container
  chooses whether to expose RepoNode remotely, register local services, or both
```

Current first step: `RepoCore` owns the object store, manifest validation, and
capacity accounting. `RepoNode` keeps the existing public API and standalone
service registration behavior, but now delegates storage work to `RepoCore`.
`RepoNode::registerLocalServices(LocalServiceRegistry&)` exposes the same
STORE/FETCH/MANIFEST/INVENTORY/CAPABILITY/DELETE handlers for trusted
in-process invocation.
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

Supported mode strings are `remote`, `embedded`, and `both`. Empty config keeps
the default `remote` behavior for existing standalone repo deployments.

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

This implementation stores reassembled object payloads. A future lower-level
optimization can store the wire encoding of received `ndn::Data` segments and
answer matching Interests by returning those Data packets unchanged. That would
avoid re-segmenting objects on cache hits, but requires exposing a packet-level
segmented-data API below the current object API.

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
