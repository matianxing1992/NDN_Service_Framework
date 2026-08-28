# T012 Public Artifact API Closure

## Outcome

`PASS`

The Python package now exposes a typed, stable artifact-manifest-v2 facade
through `RepoClient` and `ArtifactRepositoryApi`. Applications select generic
collaboration or Targeted control with public values and never manipulate
private control-mode fields or wire operations.

## Public surface

- `RepoClient.publish_file()` / `publish_file_async()`
- `RepoClient.fetch_file()` / `fetch_file_async()`
- `RepoClient.begin_upload()` / `begin_fetch()`
- `ArtifactUploadSession` and `ArtifactFetchSession`
- `ArtifactReference` validated constructor and `to_dict()`
- `ArtifactPublishResult`, `ArtifactFetchResult`, and
  `ArtifactReplicaResult`
- `ArtifactProgress` with monotonic identity/counter enforcement
- `ArtifactCancellationToken`
- `ArtifactApiError` and the 15 stable `ArtifactErrorCode` categories
- `ArtifactControlOptions` with `COLLABORATION` and explicit-provider
  `TARGETED` modes
- `ArtifactApiBackend`, `ArtifactPublishDriver`, and `ArtifactFetchDriver`
  as the public runtime adapter boundary

The operation ID is a deterministic digest of direction, caller idempotency
key, and complete immutable artifact identity. Reusing a key cannot alias a
different digest, size, root manifest, or policy epoch.

## Contract properties verified

- Sync and async publication/retrieval return stable typed results.
- Achieved durability must equal distinct committed receipt identifiers.
- Repeated publication with the same identity/key has the same operation ID.
- Matching destinations produce a verified deduplication result.
- Advanced commit and abort are deterministic and idempotent.
- Cancellation prevents new work and preserves progress only when requested.
- Progress identity, sequence, timestamp, byte counters, retransmission count,
  and replica counts are monotonic.
- A blocking progress callback cannot indefinitely block the transfer engine;
  delivery is coalesced through a one-element bounded queue.
- Timeout and unexpected backend failures map to bounded public errors.
  Unexpected raw backend diagnostics are not exposed.
- The Targeted provider is explicit and invalid control combinations fail
  before runtime dispatch.
- Package metadata exports `py.typed`.

## Verification

```text
CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 setup.py build_ext --inplace --force -j1
=> PASS

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python \
  -p 'test_spec164_public_artifact_api.py'
=> 7/7 passed

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec164*.py'
=> 60/60 passed

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python \
  -p 'test_ndnsf_repo_exact_packets.py'
=> 12/12 passed

CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 -m pip wheel . --no-build-isolation --no-deps
=> py_repoclient-0.2.0-cp38-cp38-linux_x86_64.whl

isolated --system-site-packages venv install and import
=> PASS; artifact_api.py and py.typed retained in wheel
```

## Boundary and next dependency

The facade is independent of transport implementation. A runtime backend owns
NDNSF collaboration, leases, manifest publication, segmented NDN transfer, and
repository receipts. T013 must wire maintained local and MiniNDN applications
to a concrete backend while keeping application source limited to this public
surface.

