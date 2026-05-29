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
- `RepoNode`: a repo node that can register NDNSF services on a
  `ServiceProvider`, plus local `put/get/list/remove` helpers for C++ apps and
  tests.
- `RepoClient`: high-level `put/get/list/remove` helpers, plus lower-level
  request helpers for applications that need direct `ServiceUser` control.
- `py_repoclient`: a Python binding for the reusable repo client/protocol API.

The service name is application-configurable. By default, all repo nodes share:

```text
/NDNSF/DistributedRepo
```

The operation is carried in the request payload, not in the service name. Current
operations include:

```text
CAPABILITY
STORE
STORE_FROM_NDN
STORE_MANIFEST
FETCH
FETCH_PREPARE
MANIFEST
INVENTORY
DELETE
```

Objects are described by `RepoObjectManifest`, which contains object name,
object type, SHA-256, size, segment count, replication factor, selected replica
nodes, and policy epoch. Large object transfer uses ndn-cxx Segmenter and
SegmentFetcher directly through the NDNSF Python binding, while placement and
replica selection use NDNSF service discovery and ACK metadata. This keeps the
repo generic: the payload may be a model shard, runner, ONNX file, PyTorch
artifact, activation tensor, payment-workflow record, telemetry log, JSON
configuration, or any other NDNSF application object.

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
