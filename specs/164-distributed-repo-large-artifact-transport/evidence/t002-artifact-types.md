# T002 Canonical Artifact Types Evidence

Date: 2026-07-29

## Delivered boundary

- Canonical, validated C++ types for `ArtifactReference`,
  `ArtifactCapability`, `ArtifactRootManifest`, `ArtifactManifestPage`,
  `ArtifactChunk`, `ArtifactUploadLease`, and `ArtifactReplicaReceipt`.
- Stable machine-readable error prefixes for invalid names, digests, formats,
  algorithms, limits, ranges, manifests, capabilities, leases, and receipts.
- Hard limits are checked before manifest child decoding.
- Root manifests accept only approved public-key signature algorithms; HMAC is
  not accepted as a public root signature.
- Python callers construct read-only native objects through fail-closed
  dictionary decoders; unknown fields and invalid field types are rejected.

## Verification

```text
python3 setup.py build_ext --inplace
PASS

./waf build --targets=unit-tests -j2
PASS (78 objects, build/unit-tests linked)

./build/unit-tests --run_test=DistributedRepoArtifactTypes --log_level=test_suite
PASS (3/3)

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_artifact_types.py -v
PASS (8/8)

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_spec164_legacy_subject.py -v
PASS (3/3)

PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:pythonWrapper \
  python3 tests/python/test_ndnsf_repo_chunked_file.py -v
PASS (2/2)

./waf build --targets=ndnsf-distributed-repo -j2
PASS
```

The first native build attempt failed because the unit-test target did not
include `NDNSF-DistributedRepo/include`. The target configuration was corrected
and the same build then passed. No MiniNDN, TigerCluster, network, or throughput
experiment was run for this type-contract task.
