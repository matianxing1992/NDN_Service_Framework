# py-repoclient

`py-repoclient` is the Python binding for the reusable
NDNSF-DistributedRepo client/protocol API. It is intentionally application
neutral: it exposes repo manifests, placement decisions, request encoders, and
a small Python `RepoClient` helper that can be used with the NDNSF Python
`ServiceUser`.

Install it after building NDNSF and NDNSF-DistributedRepo:

```bash
cd NDNSF-DistributedRepo/pythonWrapper
python3 -m pip install -e .
```

From the repository root, the same install can be triggered together with the
DistributedRepo build:

```bash
./waf
python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper
```

Minimal use:

```python
from ndnsf_distributed_inference import DistributedRepo

repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
)
manifest = repo.put(
    "APP/Generic/BinaryBlob/demo",
    b"...",
    object_type="binary-blob",
    replication_factor=2,
    policy_epoch="/Policy/app/v1",
)
payload = repo.get(manifest.object_name, manifest)
```

The high-level API expands relative object suffixes under the publisher
namespace, for example
`/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`.
The repo stores the app-created signed Data segments as opaque bytes; it does
not add a second encryption layer to encrypted or plaintext application data.
Those Data packet names must remain under the publisher identity so
hierarchical trust schemas can validate them against a trust-root certificate
anchor.

The namespace and trust rules are:

- `/NDNSF/DistributedRepo` is the shared service name, not the stored-data
  prefix;
- stored objects and payload segments are under the publisher identity;
- deployment config objects are under the controller publisher identity;
- project roots should be parent namespaces such as `/example/repo`;
- child certificates must be issued under that root namespace, for example
  `/example/repo/user/KEY/.../ROOT/...`.

`DistributedRepo.from_config("repo_policy.yaml")` is still useful for local
tests and deployment-side tools that already have the policy file on disk.

Use the lower-level `py_repoclient.RepoClient` only when your application
already owns an NDNSF `ServiceUser`:

```python
from ndnsf import ServiceUser
from py_repoclient import RepoClient

user = ServiceUser(...)
repo = RepoClient(user, "/NDNSF/DistributedRepo")

manifest = repo.store(
    object_name="/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Model/Shard/0",
    payload=b"...",
    object_type="model-shard",
    replication_factor=2,
    policy_epoch="/Policy/app/v1",
)
payload = repo.fetch(manifest.object_name)
```

If you already have a manifest, use the manifest-aware object helper instead:

```python
payload = repo.fetch_object(manifest)
```

This verifies size and SHA-256 from the manifest and keeps the application
independent from whether the repo stored the object as one payload or as
object-level chunks.

For large objects or APP-signed `Data` segments, higher-level packages such as
NDNSF-DistributedInference may still use the lower-level segmented Data APIs
from `ndnsf` and keep `py-repoclient` for manifest/protocol and repo service
operations.
