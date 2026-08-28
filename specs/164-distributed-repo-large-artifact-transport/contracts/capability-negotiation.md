# Artifact Capability Negotiation Contract

## Scope

This contract filters repository ACKs before placement. It is authoritative for
`artifact-manifest-v2` compatibility and does not rank RTT, bandwidth, load, or
failure domain. Placement strategies may rank only the eligible set.

`exact-packet-v1` remains a separate compatibility backend whose application-
signed Data names and wire bytes retain their existing trust semantics. The v2
negotiator never converts, downgrades, or interprets exact-packet objects.

## Advertisement

Repository ACKs and `CAPABILITY` responses carry
`servicePayloadSchema = "ndnsf-repo-capability-v2"` and one
`artifactCapability` object:

| Field | Meaning |
|---|---|
| `repoNode` | Canonical repository provider identity; duplicate identities do not count twice. |
| `formatVersions` | Explicit supported formats, currently `artifact-manifest-v2` and/or `exact-packet-v1`. |
| `digestAlgorithms` | Supported immutable content/manifest digest algorithms. |
| `signatureAlgorithms` | Supported public root-signature algorithms; shared HMAC is not valid here. |
| `maxArtifactBytes` | Maximum logical artifact size accepted by this repository. |
| `maxChunkBytes` | Maximum v2 chunk size. |
| `maxRootEncodedBytes` | Maximum encoded signed-root size. |
| `maxPageEncodedBytes` | Maximum encoded manifest-page size. |
| `maxPageEntries` | Maximum child entries in one page. |
| `maxManifestDepth` | Maximum accepted manifest depth. |
| `supportsResume` | Whether exact immutable-operation resume is implemented. |
| `supportsReplicaReceipts` | Whether authenticated durable commit receipts are implemented. |
| `policyEpoch` | Exact trust-policy epoch accepted for the operation. |

Unknown fields, duplicate list values, unknown algorithms/formats, zero limits,
and limits above the hard parser bounds fail validation. A missing
`artifactCapability` is not interpreted as v2 support.

An upgraded but otherwise unconfigured repository advertises
`formatVersions = ["exact-packet-v1"]`, `supportsResume = false`, and
`supportsReplicaReceipts = false`. Operators/runtimes must explicitly enable
the v2 format and its implemented features; code version alone is not a
capability claim.

## Request Requirements

Before manifest creation/selection, the runtime supplies:

- complete immutable `ArtifactReference`;
- root public-signature algorithm;
- planned chunk size;
- maximum encoded root and page sizes;
- maximum page entry count and manifest depth;
- whether resume is required;
- whether authenticated replica receipts are required;
- requested distinct replica count.

Every selected repository must match the exact format, digest algorithm,
signature algorithm, size and geometry limits, feature flags, and policy epoch.
Incompatibility reasons are stable bounded identifiers:

```text
format-version
digest-algorithm
root-signature-algorithm
artifact-size-limit
chunk-size-limit
root-size-limit
page-size-limit
page-entry-limit
manifest-depth-limit
resume
replica-receipts
policy-epoch
duplicate-repo-node
invalid-capability
```

## Fail-Closed Result

- no compatible repository:
  `ArtifactApiError(UNSUPPORTED_CAPABILITY)`;
- some compatible repositories but fewer distinct identities than requested:
  `ArtifactApiError(DURABILITY_NOT_ACHIEVED)` with
  `achieved_replicas = compatible distinct identities`;
- enough compatible repositories:
  `ArtifactCapabilityNegotiation`, containing all eligible capabilities and
  bounded rejection reasons; placement may then rank the eligible set.

The runtime must not reduce replica count, disable resume/receipts, change
algorithm, or choose `exact-packet-v1` automatically. An application that
intentionally preserves legacy packet wires uses
`DistributedRepo.exact_packets`, whose declared
`format_version` is `exact-packet-v1`.
