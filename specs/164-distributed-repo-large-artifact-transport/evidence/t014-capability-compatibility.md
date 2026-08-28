# T014 Capability Negotiation and Compatibility Evidence

Date: 2026-07-30  
Verdict: **PASS**  
Claim level: implemented and executed; no performance claim

## Implemented contract

`artifact-manifest-v2` now has an explicit fail-closed capability contract
across C++, pybind11, the public Python facade, Repo ACKs, and the CAPABILITY
operation:

- exact format version and content digest algorithm;
- public root-signature algorithm;
- artifact, chunk, signed-root, page, page-entry, and manifest-depth limits;
- exact policy epoch;
- resume support;
- authenticated replica-receipt support;
- requested count of distinct eligible repository identities.

The C++ authority is `ArtifactCapabilityRequirements` plus
`ArtifactCapability::incompatibilities(...)` and
`requireSupport(...)`. Python exposes
`ArtifactCapabilityRequirements` and
`negotiate_artifact_capabilities(...)`. Repository advertisements use the
typed `ndnsf-repo-capability-v2` service payload with a validated nested
`artifactCapability`.

No capability is inferred from software version or from missing fields.
Unconfigured Repo nodes advertise only `exact-packet-v1`, with resume and
replica receipts false. The generic Repo node CLI requires explicit
`--artifact-format artifact-manifest-v2`, policy epoch, resume, and receipt
claims before a node can advertise those properties.

## Fail-closed behavior

- zero eligible nodes raises `UNSUPPORTED_CAPABILITY`;
- fewer distinct eligible nodes than requested raises
  `DURABILITY_NOT_ACHIEVED` and records the achieved count;
- duplicate advertisements for one Repo identity count once;
- format, algorithm, geometry, feature, and policy mismatch produce bounded
  rejection identifiers;
- v2 negotiation rejects an `exact-packet-v1` reference instead of silently
  selecting the legacy backend;
- invalid or missing nested capability advertisements do not become v2
  candidates.

The exact wire contract and rejection identifiers are maintained in
`contracts/capability-negotiation.md`.

## Explicit legacy backend

`DistributedRepo.exact_packets` returns `ExactPacketRepositoryApi`, whose
declared format is `exact-packet-v1` and whose methods delegate to the existing
`put_signed_packets(...)` / `get_signed_packets(...)` implementation. The
legacy direct methods remain source-compatible.

The packet persistence, lookup, signature/wire bytes, names, prepared
forwarding hint, retry, and immutable conflict paths were not rewritten.
Consequently legacy packet trust remains application-signature validation over
the exact original Data wire; it is not replaced by v2 signed-root semantics.

## Build and tests

Binding build:

```bash
cd NDNSF-DistributedRepo/pythonWrapper
CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 setup.py build_ext --inplace --force -j1
```

Result: **PASS**.

C++ capability and artifact type suite:

```bash
./waf --targets=unit-tests -j2
./build/unit-tests \
  --run_test=DistributedRepoArtifactTypes/* \
  --log_level=message
```

Result: **5/5 passed**. New cases cover v2 format/signature/limits/features,
stable unsupported-capability failure, and isolation of exact-packet trust.

Python capability/compatibility contract:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec164_capability_compatibility.py
```

Result: **7/7 passed**. Coverage includes legacy-only default advertisement,
typed ACK and CAPABILITY decoding, every compatibility dimension, distinct
durability, no silent fallback, and explicit exact-packet delegation.

Legacy exact-packet regression:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_ndnsf_repo_exact_packets.py
```

Result: **12/12 passed**.

Repo capability/runtime regression:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_ndnsf_repo_ha.py
```

Result: **48/48 passed**. The stale schema-generation assertion found by the
full run was repaired to compare against the persistence authority
`SCHEMA_GENERATION`, rather than hard-coding generation 9 while the current
authority is generation 11.

All Spec 164 Python contracts:

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec164*.py'
```

Result: **72/72 passed**.

## Audit

- Context Mode remained optional and fail-closed; repository documents and
  source were authoritative.
- CodeGraph verified the C++/binding/Python call paths and legacy packet blast
  radius.
- Spec Kit strict structure scan passed before closure.
- GSD remains the resumable outer execution loop.
- ARS was not used to infer performance from contract tests.

The post-implementation audit specifically rejected automatic v2
advertisement by upgraded legacy nodes and changed the default to legacy-only.
No unresolved CRITICAL, HIGH, or MEDIUM T014 finding remains. Gate verdict:
**PASS**.
