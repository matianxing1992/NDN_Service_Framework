# ACK-Driven YOLO Contract v1

## Public request boundary

The high-level application submits:

```text
model: immutable ModelRef for canonical YOLO26n ONNX
task: object-detection InferenceTaskRef
input: ApplicationInput in INLINE or REPO_REF mode
options: TaskOptions containing only task semantics and deadline constraints
strategy: existing ModelPlacementStrategy, default PreSplitFirstStrategy
```

The public qualification path accepts no Provider list, prebuilt deployment,
split ID, or role map. Qwen generation convenience inputs adapt to the same
generic coordinator; they do not create a second planning owner.

The maintained application shape is model-first. For a generic YOLO task, the
canonical public call is `InferenceClient.request_task()`:

```python
yolo_adapter = build_yolo26n_adapter(canonical_package,
                                     registry_path=catalogue_registry)
handle = client.request_task(
    model=yolo26n_model_ref,
    task=object_detection_task,
    input=ApplicationInput.from_repo_ref(
        task_name=object_detection_task.task_name,
        input_schema_digest=yolo_adapter.descriptor.input_schema_digest,
        options_schema_digest=yolo_adapter.descriptor.options_schema_digest,
        reference=encrypted_image_repo_ref,
        options=b"{}",
    ),
    timeout_ms=60000,
    options=task_options,
    strategy=PreSplitFirstStrategy(),
)
```

The example uses `ApplicationInput.from_repo_ref(...)` deliberately: a raw
reference mapping is not a public request object and must not bypass input
schema, digest, encryption, or protection-epoch validation.

An `InferenceApplication` may expose the same request through its generic
facade, using `request(model=..., task=..., input=..., task_options=...,
timeout_ms=...)`. `InferenceClient.request_model()` is reserved for the
existing `GenerationInput`/`GenerationConfig` convenience path and is not a
generic YOLO entry point. A user-level `request_model()` call is not part of
this contract.

The application does not pass Provider names, a candidate ID, a role map, or a
prebuilt deployment plan. Those values are produced only after the closed ACK
snapshot and are visible only in the sealed plan/evidence.

File-backed `PreSplitCatalogSnapshot` values or a caller-supplied Provider-key
map may be used by focused/offline fixtures, but they are not live ACK
discovery and cannot qualify the maintained application. The live capability
authority is the immutable `CollaborationAckClosed` snapshot returned by the
existing network collaboration. `PreSplitCatalogSnapshot` is only artifact-
publication metadata for already-published role objects; it must be resolved
through an authenticated repository/data path and must not close ACK collection
or choose a candidate. The qualification caller must obtain the ACK snapshot
from the network collaboration and verify Provider offers with the existing
NDNSF Trust Schema and Provider identity/certificate verifier anchored by the
candidate-bound `SPEC180_YOLO_OFFER_TRUST_ROOT`; static files, Provider-supplied
keys, and caller HMAC maps must not replace that authority. Verification binds
the signer to the advertised Provider/service and canonical offer digest,
request/attempt, model/graph, validity window, and boot epoch.

### Semantic safe-cut contract

The signed catalogue is authoritative for a candidate's partition, but a
boundary node name by itself is not a partition proof. Every shared-backbone
candidate MUST carry canonical, sorted, duplicate-free role node sets; explicit
branch ownership; producer/consumer tensor names, shapes, and dtypes at every
cross-role edge; per-role input/output interfaces; dependency edges; and a
full-model equivalence/oracle record. The exporter and adapter MUST validate
that these fields describe the YOLO backbone/neck and Detect-scale branches
and MUST reject cuts derived only from a percentage, fixed node index, or
topological prefix. The semantic descriptor and its evidence digest are part
of the signed catalogue and candidate digest. A focused test that merely
checks that a boundary name appears in `safe_cuts` is not sufficient for local,
SIF, or Tiger qualification.

The descriptor uses schema `spec180-yolo-semantic-partition-v1` and contains
`roleNodeSets`, `branchOwnership`, `nodeBranch`, `tensorInterfaces`,
`roleInterfaces`, `dependencyEdges`, `safeCuts`, and `equivalence`. Node sets
and interface lists are sorted and duplicate-free. Each `tensorInterfaces`
entry binds one produced tensor to its producer node/role, all crossing
consumer nodes/roles, dtype, and shape. `roleInterfaces` binds every role's
non-empty graph-input/crossing-input and crossing-output/graph-output tensor
contracts. `dependencyEdges` and `safeCuts` must enumerate the same directed
role pairs and tensor sets. `equivalence` names the fixed full-model oracle
and positive `atol`/`rtol`; it is evidence metadata, not a permission to
accept a numerically mismatching assembled result.

### Authenticated ACK provenance

The existing NDN/SVS path validates the ACK Data packet with the configured
Trust Schema before its encrypted `RequestAckMessage` payload is admitted.
That validation result is part of the planning input: the C++ `ServiceUser`
and Python binding MUST carry the packet's non-secret signer identity (or
certificate/key-locator reference) and validated packet wire digest alongside
each `AckCandidate`. The projection MUST derive these values from the same
validated Data packet; Python MUST NOT reconstruct them from the ACK name,
Provider field, environment, or a caller key map.

The production V3 offer verifier receives the candidate plus this provenance.
It MUST reject absent, malformed, expired, or Provider/service-mismatched
provenance before checking the canonical `ProviderOfferV3` digest and before
the offer enters feasibility. Existing direct `handleRequestAckByName()` test
helpers may omit provenance only in explicitly marked unit fixtures; those
fixtures cannot be used for local, SIF, or Tiger qualification evidence.

`INLINE` is limited to 4096 bytes. `REPO_REF` carries no payload bytes and
binds one repository `LargeDataReference`: immutable NDN name, object
identifier, publication-manifest digest, plaintext size, ciphertext/content
digest, encryption flag,
input-schema digest, authorization scope, and protection epoch. The registered
YOLO image always uses `REPO_REF`. It is published before `REQUEST_SENT`, but
only the candidate-declared input-ingress role may fetch, decrypt, and verify it
after `SELECTION_COMMITTED`. Repository access is assignment-scoped: the
ingress role must present the signed plan/role authorization and current
protection epoch; the NDN name by itself is not a fetch or decryption
capability. The assignment carries only the capability/reference digest and
authorization lineage; raw decryption keys MUST NOT appear in the Request,
plan, assignment, logs, or evidence. Candidate Providers do not receive image
bytes in order to decide whether to ACK.

### Input-publication ownership

The maintained User owns publication of the encrypted YOLO input. It MUST call
the canonical `app_sdk.client.APPClient.publish_application_input_reference()`
method before invoking `request_task()`. The method delegates to the native
publisher, verifies the source-bound Data name, object ID, plaintext size,
content digest, authorization scope, protection epoch, encryption bit, and
canonical publication-manifest digest, and records only non-secret
`INPUT_REFERENCE_PUBLISHED` metadata. `request_task()` accepts the reference
only when its durable publication digest matches that journal record. The
lower network facade's legacy `publish_large_payload_reference()` is a
compatibility/offline helper and cannot by itself satisfy this contract.
Reading a caller-supplied JSON reference is insufficient because it does not
establish publication or complete security metadata for this request. No new
repository protocol or pre-Selection plaintext fetch is introduced; after
Selection, only the candidate-declared ingress role may fetch, decrypt, and
verify the object.

For the current NDNSF fetch primitive, the reference's `ciphertextDigest`
compatibility field carries the SHA-256 of the plaintext returned after
decryption; it is not a claim about the encrypted segment wire bytes. The
publication-manifest digest and the authenticated request bind this meaning.

The backward-compatible wire encoding is fixed as follows:

```text
ApplicationInput.transport_mode = INLINE | REPO_REF
ApplicationInput.large_data_reference = {} | canonical reference map

DIRequestEnvelopeV2.input_payload_b64
  INLINE   -> canonical base64(payload)
  REPO_REF -> empty string

DIRequestEnvelopeV2.task.input_transport
  -> mode, inputSchemaDigest, plaintextDigest, protectionEpoch
  -> reference fields only for REPO_REF
```

`input_manifest_digest` covers the mode, schema, plaintext digest, options
digest, protection epoch, and complete canonical reference. Existing callers
that omit the new fields adapt to `INLINE`; an empty inline payload is distinct
from `REPO_REF`.

## Required event order

For each request, evidence MUST show:

```text
INPUT_REFERENCE_PUBLISHED
  < REQUEST_SENT
  < ACK_CLOSED
  < GRAPH_READY
  < PLACEMENT_DECISION
  < ARTIFACTS_READY
  < PLAN_SEALED
  < SELECTION_COMMITTED
  < first Provider role execution
  < TERMINAL_RESPONSE
```

`INPUT_REFERENCE_PUBLISHED` is required only for `REPO_REF`; an `INLINE`
request begins at `REQUEST_SENT`.

Graph inspection and safe catalogue verification may validate static metadata
before publication as an input-safety check, but the request's candidate
records, candidate enumeration, feasibility evaluation, Provider assignment,
and final plan occur only after `ACK_CLOSED`. A preflight descriptor may retain
only the opaque signed revision/digest for reproducibility; it is not a planner
input and cannot expose candidate requirements or choose a role owner.

ACK closure is the registered 1500 ms timeout boundary. The qualification path
sets no caller-supplied `ack_coverage_roles` and no offline candidate/role
coverage predicate. Role coverage becomes meaningful only after the closed
snapshot is paired with the post-closure candidate catalogue.

Each candidate is checked against its own roles, dependency graph, runtime, and
resource requirements. A catalogue-order permutation must not affect
feasibility or the decision. `candidates[0]` and a legacy global
`required_roles` value are not placement authority. The closed snapshot is
canonicalized by Provider identity and ACK identity before its digest is
computed; arrival order is retained only as diagnostic evidence.

If both registered candidates are feasible, the adapter-declared priority
selects `shared-backbone-two-shard-v1` before `atomic-v1`; equal priorities are
resolved by candidate digest. This preference is part of the candidate and
plan identity and is tested independently of catalogue order.

## Candidate catalogue

The canonical catalogue is signed by the configured adapter/catalogue authority.
Its signature covers the model and graph identities, every candidate's complete
role/dependency/object description, ingress/egress roles, safe cuts, selection
priority, merge kind/schemas, and catalogue revision. The verifier accepts only
the configured signer key and rejects missing, unknown, or invalid signatures
before candidate enumeration. The signer key ID/public-key digest is part of the
canonical package manifest and candidate seal; ambient keys or profile-only
overrides are not accepted. A priority value without this signature is not
trusted input.

### `atomic-v1`

- Roles: `FullModel`
- Dependencies: none
- Feasible only when one ACK advertises compatible ONNX Runtime, sufficient
  device memory, the required model/task ABI, and the whole-model role.

### `shared-backbone-two-shard-v1`

- Roles: `BackboneNeck`, `DetectShard0`, `DetectShard1`, `Merge`
- Dependencies:
  - `BackboneNeck -> DetectShard0`
  - `BackboneNeck -> DetectShard1`
  - `DetectShard0 -> Merge`
  - `DetectShard1 -> Merge`
- Feasible only when four distinct ACK identities cover all four complete
  roles. The three model roles require compatible GPU execution; `Merge` is
  declared CPU postprocessing and must not execute model layers.
- `input_ingress_role`: `BackboneNeck`
- `result_egress_role`: `Merge`
- `merge_kind`: `NATIVE_POSTPROCESS` (no model-layer objects); an
  `ONNX_MERGE_GRAPH` is permitted only when its canonical graph/object digest
  is present in the package manifest and the Provider executes that graph as
  the declared merge role, never as a replacement for a GPU model role.

The two Detect roles partition the model's Detect-scale branches. They are
parallel shards of one detection stage, not two independent model heads.

### Candidate ingress/egress contract

`atomic-v1` declares `FullModel` as both `input_ingress_role` and
`result_egress_role`. `shared-backbone-two-shard-v1` declares `BackboneNeck`
and `Merge`, respectively. Only the ingress role may fetch the registered
invocation input; only the egress role may publish the terminal result. These
fields are part of the candidate digest and the sealed plan, so an assignment
cannot silently move input or result ownership to another role.

The canonical manifest owns exact tensor names, dtypes, shapes, output ordering,
initializer slices, and merge/postprocessing semantics. A candidate is invalid
if any required graph value, initializer, or output cannot be proven.

## Plan binding

The plan digest covers:

- request, attempt, invocation, deadline, and service;
- closed ACK snapshot digest;
- model/source/graph/adapter/catalog/candidate/strategy identities;
- catalogue signer key identity, catalogue revision, and signature digest;
- adapter candidate priority and the resolved tie-break outcome;
- complete one-to-one role map and dependency graph;
- candidate `input_ingress_role` and `result_egress_role`;
- task input and preprocessing identity;
- input transport/reference, security domain, and protection epoch, but never
  input plaintext;
- canonical artifact references and assembly assignment digests;
- result schema, merge/postprocessing identity, and numerical oracle identity.

Every role appears once and every selected Provider appears once. An invalid
mapping fails before Selection.

The sealed plan also materializes the candidate dataflow contract: one
`APPLICATION_INPUT` endpoint whose consumer is exactly `input_ingress_role`,
and one terminal-result owner equal to `result_egress_role`. The endpoint has
no producer role, carries the request-bound input/reference digest, and cannot
be reassigned by a Provider ACK, catalogue order, or a legacy role hint.

## Role assembly and execution

Each Provider receives only its signed `RoleAssemblySpec`. Before computation it
must verify the full plan binding, its identity and role, the deadline, replay
state, every canonical object name/range/byte-count/digest, and the declared
runtime. A Spec180 qualification assignment must carry a protected,
non-`plaintext-v1` protection epoch; the source-compatible default is rejected
before local, SIF, or Tiger evidence. A cache hit must satisfy the same identity.

Role validation is kind-specific. `PIPELINE_RANGE`, `TENSOR_RANK`, and
`HYBRID_RANK` require a valid layer interval. `COMPONENT_SET` requires a
non-empty, sorted, duplicate-free canonical node set and does not invent a
Transformer layer interval; its wire sentinel is exactly
`layer_begin=0, layer_end=0`. Python, native, and binding validators must agree.
Cache identity includes adapter/assembler ABI, role kind, object set, security
domain, and protection epoch. Every cache hit rechecks current-request
authorization; a stale or revoked epoch fails closed.

Only the declared `input_ingress_role` fetches the invocation input from its
authenticated encrypted repository reference. The production Provider context
exposes this boundary as `fetch_application_input()`; a non-ingress role fails
closed with a structured `DI_INPUT_FETCH_ROLE_MISMATCH` reason. Intermediate
dependencies use the existing NDNSF confidentiality path. The production
terminal boundary is `publish_terminal_result(...)`; a non-egress role fails
closed with `DI_TERMINAL_RESPONSE_ROLE_MISMATCH`. The terminal result remains
an encrypted, authorized NDNSF Response. Plaintext input/result bytes and
secret capabilities must not appear in logs or evidence.

Dependencies are exact-name NDN Data bound to request, attempt, plan, source
role, destination role, and tensor identity. The merge role waits for both
declared head outputs and rejects missing, duplicated, wrong-plan, or
wrong-shape inputs.

## Numerical oracle

Atomic and shared-backbone cases use the same canonical input,
preprocessing, output ordering, merge, and postprocessing. Compare corresponding
numeric outputs with `atol=1e-3` and `rtol=1e-4`; detection set ordering is
canonicalized before comparison. A missing/extra detection or schema mismatch
fails regardless of numeric tolerance.

## Negative cases

The contract requires focused tests for late/duplicate ACKs, insufficient
capability, stale cache claims, uncertified candidate, unsafe cut, duplicate or
missing role owner, altered assignment, wrong Provider, missing/truncated/wrong
object, shape/dtype mismatch, wrong-plan dependency, incomplete merge, and
post-terminal output. It also requires candidate-order permutation, oversized
inline input, plaintext/altered/unauthorized input reference, non-selected input
fetch, mismatched or revoked protection epoch, cross-security-domain cache hit,
`COMPONENT_SET` with a fabricated range or invalid node set, range role without
a valid interval, and plaintext-log detection.
