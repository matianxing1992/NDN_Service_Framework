# Contract: Canonical Artifact and Provider Assembly V1

**Status**: Planned  
**Owner**: NDNSF-DistributedInference over NDNSF-DistributedRepo

## Identity Contract

```text
ModelIdentity
  source/publisher provenance
  normalized tensor-map digest
  configuration/semantics/graph/tokenizer digests

CanonicalArtifactProfile
  layerizer/adapter + serializer/schema + chunk/layout
  precision/format + protection transform/epoch

CanonicalLayerIdentity
  ModelIdentity + CanonicalArtifactProfile
  component kind + stable ordinal/graph region
  tensor/chunk manifest + content digest
```

Request, attempt, Provider, role, pipeline boundary, tensor degree, rank, and
placement strategy never enter canonical layer equality.

## Namespace Shape

For public artifacts:

```text
/<publisher>/NDNSF-DI/MODEL/v1
  /NAME/<human-readable-model-components...>
  /MID/<model-identity-digest>
  /PROFILE/<artifact-profile-digest>
  /MANIFEST/<model-manifest-digest>
  /LAYER/<component-kind>/<stable-coordinate>/MANIFEST/<layer-manifest-digest>
  /OBJECT/<object-digest>/<segment-number>
```

Human-readable components aid diagnosis but never decide equality. Protected
profiles may replace readable/correlatable components with policy-domain opaque
or keyed names while their authorized manifests preserve exact internal identity.
This is the only V3 name grammar. `REV/CFG/CONTENT` variants are invalid V3
aliases rather than equivalent spellings.

## Publication

Publication is content-addressed, idempotent, resumable, and root-last:

1. normalize and verify source model identity;
2. run a locally installed, digest-pinned layerizer/adapter;
3. publish immutable tensor/chunk objects;
4. publish and verify canonical layer manifests;
5. publish transformation attestation binding source, tool/profile, and output;
6. activate the signed root model manifest only after complete durable cover.

Equal content converges. A semantic-name/content conflict, missing object,
oversized declaration, invalid attestation, or partial root fails closed.

## Selective Retrieval

A Provider resolves a signed root/layer manifest and fetches only objects needed
by its sealed `RoleAssemblySpec` that are not already locally verified.

- If a full logical layer fits the staging envelope, full-layer retrieval is a
  valid compatibility path.
- If it does not fit, independently verifiable tensor/chunk/range retrieval is
  mandatory for that plan to be feasible.
- Retrieval layout may change transfer efficiency but never canonical layer
  identity.
- Manifest and object verification use the existing Spec 164 public artifact
  API; this feature does not introduce per-packet public-key verification.

## Role Assembly

```text
RoleAssemblySpec {
  modelManifestDigest
  artifactProfileDigest
  graphDigest
  adapterDescriptorDigest
  assemblerDescriptorDigest
  componentSelectors[]
  tensorDistributions[]
  precision / quantization / layout / padding
  backendAbi
  rank / collective / redistribution contracts
  expectedInputOutput
  resourceEnvelope
}
```

The specification is declarative data. It cannot carry executable code or a
path chosen by an external strategy.

Provider preparation uses:

```text
verify assignment and manifests
-> resolve exact local inventory
-> single-flight missing object fetches
-> verify complete inputs
-> single-flight assembly by assembly-spec digest
-> private temporary output
-> complete validation
-> atomic fragment activation
-> load to selected CPU/device set
-> local-ready
```

No global GPU lock is held during fetch or assembly. Cancellation may retain
fully verified canonical objects/fragments; partial output is resumed,
quarantined, or removed and is never advertised as a hit.

## Residency Levels

### Canonical layer residency

Exact verified canonical objects on disk/RAM. Reusable across compatible
placement plans and tensor-degree vectors.

### Assembled fragment residency

Exact assembly identity:

```text
model/profile + RoleAssemblySpec + adapter/assembler
+ backend ABI + precision/quantization + protection epoch
```

It may be portable to a compatible same-architecture device and therefore does
not necessarily bind a physical device UUID.

For the ONNX baseline, one completed Provider-local role/rank assembly is stored
durably as one immutable content-addressed
`<assembled-object-digest>.ndnsf-onnx-artifact` bundle. Its meaningful NDN
identity is:

```text
/<provider>/NDNSF-DI/ASSEMBLED/v1
  /NAME/<model-name...>
  /MID/<model-identity-digest>
  /PROFILE/<artifact-profile-digest>
  /GRAPH/<graph-digest>
  /ROLE/<role-kind>/<semantic-coordinate...>/RANK/<rank>-OF-<degree>
  /RECIPE/<role-assembly-spec-digest>
  /OBJECT/<assembled-object-digest>
```

The bundle contains an embedded signed manifest and one of two deterministic
payload layouts:

```text
INLINE_ONNX:
  model.onnx

ONNX_EXTERNAL_DATA:
  model.onnx
  model.onnx.data   # all tensors in one external-data file
```

The second layout is mandatory when the generated ModelProto cannot be safely
serialized/checked as an inline ONNX model (including the ONNX/protobuf large-
model bound). The embedded manifest binds the storage mode, canonical layer
manifest/digest set, ordered layer or component selectors, tensor distribution,
adapter/assembler descriptor, backend ABI, precision/quantization, ordered entry
names/lengths/digests, ONNX checker result, and protection epoch.

`AssembledOnnxArtifactV1` has deterministic uncompressed framing:

```text
magic = "NDNSFONNXA1"
formatVersion = 1
canonical signed-manifest length + bytes
entryCount = 1 or 2
ordered entry table: kind, fixed safe name, uint64 offset, uint64 length, sha256
ordered raw entry bytes
```

`INLINE_ONNX` has only `MODEL_PROTO:model.onnx`.
`ONNX_EXTERNAL_DATA` additionally has
`EXTERNAL_DATA:model.onnx.data`, and every ONNX external-data location must be
exactly that relative basename. Entries are ordered by enum, uncompressed to
keep byte identity and bounds deterministic, and their ranges may not overlap
or escape the file. The embedded signature covers the canonical manifest and
entry digests; it deliberately does not contain the whole-file digest. After
final serialization, `assembled-object-digest = SHA-256(exact bundle bytes)` is
used in the NDN `OBJECT` component and local filename, avoiding a self-referential
digest/signature cycle.

The Provider identity that performed the assembly signs the embedded
`AssembledBundleManifestV1`. Its certificate name and validated identity must
match the `<provider>` name prefix, and the manifest binds the verified origin
and layerizer-attestation digests rather than replacing either trust chain.
Default local reuse accepts only bundles signed by the same configured Provider
identity. Importing another Provider's assembled bundle is disabled unless an
operator-installed trust rule explicitly authorizes that signer, adapter/
assembler descriptor, protection domain, and ABI; equal entry or object bytes
alone never confer assembly trust.

For an unprotected profile, entry payloads may be stored as verified plaintext.
For a protected profile, a durable assembled bundle is permitted only when the
grant/policy allows `DISK_CIPHERTEXT_ASSEMBLED`; every entry payload is AEAD
encrypted at rest. The per-assembly key is derived inside the trusted Provider
boundary as:

```text
K_bundle = HKDF(epochContentKey,
  "NDNSF-DI/assembled/v1" || modelManifestDigest
  || roleAssemblySpecDigest || storageProfileDigest)
K_entry = HKDF(K_bundle, entryKind)
```

The manifest binds the AEAD/KDF identifiers, KDF-context digest, entry nonce,
ciphertext length, and ciphertext digest. A `(K_entry, nonce)` pair is used
exactly once; rebuild with different plaintext under an existing exact assembly
identity is a semantic conflict, not an overwrite. Whole-file identity is over
the final ciphertext bundle. The manifest and NDN name expose no plaintext key.
If ciphertext disk residency is not authorized, only ephemeral plaintext
materialization is allowed and no assembled disk hit may be advertised.

A content-addressed local catalog maps the NDN name to
`<cache-root>/assembled/<assembled-object-digest>.ndnsf-onnx-artifact`; the long
NDN name is never used directly as an unsafe filesystem path.

Before execution, the Provider verifies the entire bundle, enforces declared
entry-count/byte/expansion bounds, rejects absolute/up-level paths, symlinks,
duplicate names, executable entries, or undeclared content, and atomically
materializes the ONNX entry set into a private container-lifetime directory.
Protected materialization first validates a current Provider-bound grant,
decrypts only into allocations registered by `PlaintextLeaseRegistry`, and
zeroizes/deletes them on eviction, expiry, revocation, failure, or container
exit before reporting cleanup complete.
Large models are checked by filesystem path with their external data colocated.
The materialized directory may stay hot while the container lives but is scratch
and is cleaned on eviction/exit; the durable cache still contains one assembled
bundle file. Publication is not required for local reuse, but identity and
verification rules are identical whether local-only or later published.

`role-kind` is one of adapter-defined, digest-pinned
`PIPELINE_RANGE | TENSOR_RANK | HYBRID_RANK | COMPONENT_SET`. Its semantic
coordinate contains canonical layer indices/range or a canonical ordered
component-set digest, never `requestId`, `attemptId`, an arbitrary stage label,
or a filename. Human model name aids discovery; `MID`, graph, recipe, and object
digests establish equality. Thus the same semantic role can be found again
without allowing a human alias to create a false cache hit.

Canonical layer files and verified assembled files survive request completion
under the bounded Provider disk-cache policy. Container-private scratch is
deleted at container exit. Cross-container reuse is allowed only through an
explicitly mounted, bounded cache volume whose ownership, quota, protection,
and garbage-collection policy are operator configured; no implementation may
mistake ephemeral container storage for durable Repo state.

### Loaded runtime residency

Adds:

- exact ordered device set and topology digest;
- device architecture and driver/runtime/kernel/compile profile;
- Provider boot and process/runtime generation;
- communicator/collective epoch where applicable;
- reusable-state/KV contract.

Only this level may claim zero-load accelerator readiness. A device-set change
invalidates the loaded-runtime hit but may leave disk/RAM fragment reuse valid.

## Inventory in ACK Offers

ACKs carry bounded summaries and exact proof paths, not unbounded object lists.
Allowed forms include catalog/profile digest plus verified layer ranges/bitmap,
or an inventory root with requested proofs. Probabilistic summaries are hints
only and cannot establish an exact hit.

## Security

- Verify original publisher provenance separately from layerizer transformation
  attestation.
- Enforce authorization-bound key delivery and protection epochs.
- Allow only local digest-pinned adapter/assembler implementations.
- Validate every declared tensor shape, size, path, resource peak, and complete
  cover before allocation/assembly.
- Define host/device plaintext lease, eviction, revocation, and zeroization.
- Keep immutable base layers, immutable adapter/LoRA overlays, and mutable
  request/session state in separate identities/manifests.

### Protected profile key contract

Protected canonical objects remain encrypted at rest and in Repo. The artifact
policy authority issues this declarative grant after the placement/security
core is sealed but before the final plan identity exists:

```text
KeyGrantV1 {
  grantDigest
  policyAuthority
  providerIdentity
  requestId / attemptId / planCoreDigest
  modelManifestDigest / protectionEpoch
  keyId / wrappedContentKey
  allowedResidencyTiers[]
  issuedAt / expiresAt / revocationSequence
  activeRequestPolicy: CANCEL_IMMEDIATELY
  authoritySignature
}
```

`wrappedContentKey` is encrypted to the selected Provider identity certificate.
Final Selection carries only the grant name/digest and the final plan digest.
The Provider fetches,
verifies, and unwraps it inside the trusted Provider boundary. ACKs, planning
views, public manifests, Repo indexes, and strategy evidence never contain the
plaintext key or unwrapped secret. A grant for another Provider, request,
attempt, plan core, model, or epoch is unusable.

Grant acquisition and final sealing use a non-circular two-digest protocol:

```text
PlanSealerV3.sealCore(...) produces immutable PlacementPlanCoreV3
  and planCoreDigest (assignments + offers + protection requirements)
-> PlanSealerV3.grantView(core, provider) produces ProviderGrantViewV1
-> Requester sends signed GrantRequestV1 to configured ArtifactPolicyAuthority
-> authority verifies requester/model authorization, selected Provider identity,
   sealed core/grant-view digest, offer, protection policy, and current
   revocation state
-> authority returns signed Data whose KeyGrantV1 content is encrypted to that
   Provider certificate and binds planCoreDigest
-> Requester verifies the complete grant cover
-> PlanSealerV3.finalizeSecurity(core, sorted grant name/digest bindings,
   securityPolicySnapshotDigest) produces final PlacementPlanV3 and planDigest
-> PlanSealerV3 projects the final plan; Requester places only the Provider's
   grant name/digest plus planCoreDigest and planDigest in final Selection
-> Provider fetches the same Data and decrypts under its identity
```

The final identity is
`planDigest = H(canonicalPlanCoreBytes || canonicalSortedGrantBindings ||
securityPolicySnapshotDigest)`. The sorted bindings contain one entry for every
selected protected Provider and contain no secret. For an unprotected plan the
grant-binding list is empty, but finalization still uses the same algorithm.
Neither a grant nor a final plan may be substituted without changing
`planDigest`; a grant request cannot depend on that final digest, so no digest
cycle exists.

Grant Data uses:

```text
/<authority>/NDNSF-DI/KEY-GRANT/v1
  /PROVIDER/<provider-identity-digest>
  /REQ/<request-id>/ATTEMPT/<attempt-id>/PLAN-CORE/<plan-core-digest>
  /MODEL/<model-manifest-digest>/EPOCH/<protection-epoch>
  /GRANT/<grant-digest>
```

The authority identity/trust schema and endpoint are operator configuration, not
strategy input. Failure to obtain every selected Provider grant prevents final
Selection publication. The authority also publishes signed
`RevocationStateV1(policy, model, protectionEpoch, revocationSequence,
notBefore, nextCheckAt)`. A Provider validates it at grant unwrap, before every
loaded-runtime reuse/JIT admission, and at `nextCheckAt` while active. Expiry or
an observed higher revocation sequence prevents new use and triggers the signed
active-request policy. Network failure past `nextCheckAt` fails closed; no stale
grant grace period is implicit.

Protected runtime state follows:

```text
NO_GRANT
  -> GRANT_VERIFIED
  -> HOST_PLAINTEXT_LEASED
  -> DEVICE_PLAINTEXT_LEASED
  -> DRAINING
  -> ZEROIZED

any live state -> REVOKED -> DRAINING -> ZEROIZED
any validation/cleanup error -> FAILED_CLOSED
```

Every host/device plaintext allocation is registered by allocation ID, address/
device handle, byte bound, grant, protection epoch, owner runtime, and fencing
token before exposure. Expiry, revocation-sequence advance, plan replacement,
Provider restart, device loss, eviction, or protection-epoch rotation fences the
loaded-runtime identity and prevents new work. `CANCEL_IMMEDIATELY` cancels active
work, releases collectives, overwrites registered host buffers, invokes the
backend's device-buffer zeroization/synchronization primitive, destroys the
runtime context when direct overwrite cannot be proven, and only then records
`ZEROIZED`. Encrypted canonical objects may remain; plaintext fragments and
loaded runtimes may not.

Negative tests cover wrong recipient, stale grant, grant replay, changed
revocation sequence, epoch cross-hit, post-restart reuse, incomplete buffer
registry, zeroization failure, and an old runtime advertised after revocation.

## Compatibility

V2 preassembled role packages are a permanent explicit
`PREASSEMBLED_PARTITION_SINGLE_DEVICE` compatibility profile for Spec 170, not a
fallback or migration stage. Their role/partition identity never substitutes for
canonical layer equality, and V2 loaded runtime entries never cross-hit V3.
