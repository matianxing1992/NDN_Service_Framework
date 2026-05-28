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
from ndnsf import ServiceUser
from py_repoclient import RepoClient

user = ServiceUser(...)
repo = RepoClient(user, "/NDNSF/DistributedRepo")

manifest = repo.store(
    object_name="/APP/Model/Shard/0",
    payload=b"...",
    object_type="model-shard",
    replication_factor=2,
    policy_epoch="/Policy/app/v1",
)
payload = repo.fetch(manifest.object_name)
```

For large objects or APP-signed `Data` segments, higher-level packages such as
NDNSF-DistributedInference may still use the lower-level segmented Data APIs
from `ndnsf` and keep `py-repoclient` for manifest/protocol and repo service
operations.
