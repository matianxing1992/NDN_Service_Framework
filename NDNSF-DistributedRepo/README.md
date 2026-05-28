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
  `ServiceProvider`.
- `RepoClient`: helper APIs for applications that use `ServiceUser` to store
  or fetch repo objects.
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
```

Objects are described by `RepoObjectManifest`, which contains object name,
object type, SHA-256, size, segment count, replication factor, selected replica
nodes, and policy epoch. Large object transfer uses ndn-cxx Segmenter and
SegmentFetcher directly through the NDNSF Python binding, while placement and
replica selection use NDNSF service discovery and ACK metadata. This keeps the
repo generic: the payload may be a model shard, runner, ONNX file, PyTorch
artifact, activation tensor, payment-workflow record, telemetry log, JSON
configuration, or any other NDNSF application object.

The Python-facing generic object API is:

```python
from py_repoclient import RepoClient

repo = RepoClient(user, "/NDNSF/DistributedRepo")
manifest = repo.store(
    object_name="/APP/Generic/BinaryBlob/demo",
    payload=payload,
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/example/v1",
)
payload = repo.fetch(manifest.object_name)
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
