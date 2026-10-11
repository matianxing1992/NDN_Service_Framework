# py-repoclient

`py_repoclient` is a thin pybind projection of the C++ Repo API. C++ owns artifact
validation, local storage, Persistent operations, receipts, availability, and
error results. Python converts paths and retains the native `ServiceUser` owner.

## Build and install

From the repository root, install the editable wrapper after its native
dependencies and `ndnsf` extension have been built. Host builds pin the
installed NDNSF libraries by digest before setuptools compiles the binding:

```bash
export NDNSF_LIBRARY_DIR=/usr/local/lib
export NDNSF_GLOBAL_NATIVE_DIGESTS="$(python3 -c 'import hashlib,json; names=("libndn-service-framework.so","libndnsf-distributed-inference.so"); print(json.dumps({n:hashlib.sha256(open("/usr/local/lib/"+n,"rb").read()).hexdigest() for n in names}, sort_keys=True))')"
python3 -m pip install -e NDNSF-DistributedRepo/pythonWrapper
```

The extension links the C++ `ArtifactRepositoryApi`; it does not install a
Python filesystem backend or Repo service handler.

## In-App API

```python
import hashlib
from pathlib import Path

from py_repoclient import ArtifactReference, ArtifactRepositoryApi

source = Path("model.onnx")
payload = source.read_bytes()
digest = hashlib.sha256(payload).hexdigest()
reference = ArtifactReference(
    "/models/example",
    digest,
    len(payload),
    f"/example/publisher/NDNSF/ARTIFACT/{digest}/root",
    "/example/publisher",
    "policy-epoch-1",
)

api = ArtifactRepositoryApi.make_in_app(
    durable_root="/var/lib/ndnsf-artifacts",
    owner_id="/example/application",
)
published = api.publish_file(source, reference)
api.fetch_file(reference, Path("cache/model.onnx"))
receipt = published.receipt
handle = published.handle
```

`ArtifactReference`, `ArtifactHandle`, receipts, operation status, capability
requirements, selected targets, and result values are native C++ types. Their
JSON methods use the same strict codecs as C++.

## Persistent client lifetime

Construct the framework `ServiceUser` over an application-owned `Face` whose
event loop is already running on another thread. Calls release the Python GIL
while waiting. The native Repo API retains its `NativeServiceUser` owner for its
full lifetime. Stop and join the Face loop before releasing the service user;
destroy the Repo API first.

```python
api = ArtifactRepositoryApi.make_persistent(
    service_user,
    ack_timeout_ms=1000,
    timeout_ms=30000,
)
```

The standalone Persistent server is the C++ `DistributedRepoServer`. Python
does not register Repo handlers or own a network backend.

`RepoClient(service_user)` remains as a thin Persistent convenience facade over
`ArtifactRepositoryApi`. It preserves `ack_timeout_ms=1000` and
`timeout_ms=30000`, requires explicit native placement for publication, and
delegates fetch/archive/status calls to the same C++ API. Legacy object-store,
placement, and backend-injection operations raise a versioned compatibility
error. The old async upload/fetch helpers are retired because their
cancellation tokens had no native C++ equivalent.

## Retired Python APIs

Legacy imports such as `RepoNodeApp`, `NetworkDistributedRepoClient`,
`FilesystemArtifactApiBackend`, upload/fetch sessions, backend injection, and
Python placement helpers remain as compatibility names where needed. Calling a
retired feature raises `RepoCompatibilityError` with code
`SPEC192_COMPATIBILITY_V1`, the operation name, and a native replacement path.
