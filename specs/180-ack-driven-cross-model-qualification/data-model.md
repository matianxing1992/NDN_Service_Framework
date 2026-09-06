# Data Model: ACK-Driven Cross-Model Qualification

## CanonicalModelPackage

Represents one immutable, Provider-independent model source.

- `model_family`, `model_name`, source URI/revision, source checkpoint digest,
  exporter dependency lock digest, export command/options, ONNX opset, dtype,
  batch size, and static input shape
- `model_digest`, `graph_digest`, `weights_digest`
- canonical graph and external-object NDN names, byte counts, and digests
- input/output schemas
- preprocessing and postprocessing identities
- adapter identity and safe-candidate catalogue digest
- configured catalogue signer key identity, signature algorithm, and signature
  digest; the signer identity is registered in
  `contracts/trust-root-registry-v1.json`, is part of the package manifest, and
  cannot be supplied by an ambient runtime/profile override
- exact catalogue APP Data name below `/<signer>/NDNSF/DI/`; the name prefix
  before `/NDNSF/DI/` must equal the signer identity so the native
  `ServiceUser.publish_signed_app_data()` and exact-name fetch paths agree
- external artifact-manifest authority/key identity and manifest signature
  digest for model packages staged outside the SIF; this is verified through
  `contracts/model-manifest-trust-v1.md` and the corresponding registry entry;
  it is distinct from the YOLO catalogue signer
- no Provider identity or final role assignment

Validation: every digest is canonical and content-addressed; referenced objects
exist and verify; the offline export is reproducible from the recorded source
and dependency lock; model and adapter revisions are explicit.

## CatalogueTrustRoot

The sole authority for the signed YOLO candidate catalogue.

- `authority_id`, `key_id`, public-key algorithm, signature algorithm;
- canonical public-key bytes or path plus public-key SHA-256 digest;
- catalogue schema/revision and accepted model/graph family; and
- the canonical covered-field set, including candidate roles, dependencies,
  safe cuts, ingress/egress, priority, and merge semantics.

The private key is an explicit export-time secret input only. It is never
committed, embedded in the SIF, supplied by an ACK/profile, or written to
evidence. Before ACK closure, only opaque catalogue integrity may be checked;
candidate records are unavailable to placement until the snapshot closes.

## RuntimeActiveCatalogSnapshot

This is a separate signed APP Data record from the package's
`spec180-yolo-catalogue-v1` candidate catalogue. The package record certifies
candidate topology and semantics; it does not publish runtime artifact names.
The active record uses the `ndnsf-di-presplit-catalog-snapshot-v1` envelope and
contains one ACTIVE `PreSplitCatalogSnapshot` for each registered candidate.
Each snapshot binds the candidate, model, graph, semantics, backend, precision,
snapshot digest, and every role/rank `artifact_data_names` entry to an already
published or independently verified canonical object. It is resolved by exact
APP Data name and controller signer only after ACK closure. A package catalogue,
legacy deployment manifest, offline JSON file, or generated fallback name is
not a runtime snapshot and cannot authorize Selection.

## InvocationInput

Canonical application input with exactly one transport mode.

- common identity: task name, input/options schema digests, metadata digest,
  protection epoch, and logical plaintext digest
- `INLINE`: at most 4096 payload bytes carried in the Request
- `REPO_REF`: no inline payload; immutable NDN object name, repository manifest
  digest, plaintext size, content digest, encryption flag, and authorization
  scope. In the current implementation the legacy wire key
  `ciphertextDigest` is a compatibility alias for the SHA-256 digest of the
  plaintext returned after authenticated decryption; it is not an encrypted
  segment-wire digest.

The registered YOLO image always uses `REPO_REF`. It is stored before Request
publication, but fetched only after Selection by the candidate-declared
input-ingress role. The selected role must use a reference-aware repository
primitive that verifies the manifest/content digest and encrypted authorization
before returning plaintext; a name-only or size-only fetch is insufficient.
If that verification is unavailable, the request fails closed. Input plaintext
is excluded from ACKs, plans, logs, profiles, and evidence.

## RoleDataflowBinding

The post-ACK placement projection makes application-input and terminal-result
ownership explicit instead of inferring either one from role names or catalogue
order.

- exactly one `TensorEndpoint` with `source_kind=APPLICATION_INPUT`;
- `producer_role` is empty for that endpoint;
- `consumer_role` equals the candidate's signed `input_ingress_role`;
- the endpoint binds the request/attempt, input transport mode, schema digest,
  plaintext digest, reference/content digest, and protection epoch; and
- exactly one `terminal_response_owner` equals the candidate's signed
  `result_egress_role`.

The binding is included in the sealed plan digest and in each signed role
assignment. A Provider context may call `fetch_application_input()` only when
its role equals the endpoint consumer, and may call
`publish_terminal_result(...)` only when its role equals the terminal owner.
Rejected calls produce stable non-secret reasons
`DI_INPUT_FETCH_ROLE_MISMATCH` or `DI_TERMINAL_RESPONSE_ROLE_MISMATCH` and do
not fetch, decrypt, publish, or compute.

## SafeExecutionCandidate

One adapter-certified topology that may be considered after ACK closure.

- `candidate_id`, `candidate_digest`
- signed `selection_priority` used only after feasibility; equal priorities use
  the candidate digest as a stable tie-break
- catalogue signer key identity and signature; the signature covers the
  canonical candidate entry, including roles, dependencies, ingress/egress,
  safe cuts, priority, merge semantics, and model/graph identities
- complete role set and one directed acyclic dependency graph
- exactly one `input_ingress_role` and one `result_egress_role`
- semantic partition proof: canonical role node sets, branch ownership,
  producer/consumer tensor names/shapes/dtypes at cross-role edges, and the
  per-role input/output interfaces; a boundary node name or graph percentage
  is not sufficient
- per-role input/output tensor interfaces
- per-role initializer/object slices
- runtime, memory, device, and role capability requirements
- merge kind, merge input/output schemas, and result semantics; a native
  postprocessing merge has no model-layer objects, while an ONNX merge graph
  must name and digest that graph explicitly

Required YOLO candidates: `atomic-v1` and `shared-backbone-two-shard-v1`.
Each candidate declares exactly one `input_ingress_role` and one
`result_egress_role`; the shared candidate uses `BackboneNeck` and `Merge`,
while the atomic candidate uses `FullModel` for both.

## ProviderCapabilityOffer

Authenticated, request-scoped view derived from one successful ACK.

- request, attempt, Provider, service, and deadline identity
- runtime/backend and model-format compatibility
- available host/device memory and compute/device identity
- supported roles/operations
- cached canonical object/assembled-role identities
- queue/admission metadata used by the selected strategy
- signature and replay lineage
- authenticated ACK provenance: signer identity (or certificate/key-locator
  reference) and the validated ACK Data wire digest, projected by the real
  ServiceUser/pybind path rather than reconstructed by the caller

The offer is advisory until identity and cache objects verify.

## AckSnapshot

Immutable set of valid offers closed for one request.

- request and attempt identity
- canonical Provider/ACK identity ordering (arrival order is diagnostic only)
- the authenticated signer provenance associated with each accepted ACK;
  missing or Provider-mismatched provenance is not a valid offer
- closure time and digest
- rejected/late/duplicate ACK record

State: `OPEN -> CLOSED`; it never reopens. Planning accepts only `CLOSED`.

## PlacementDecision

- ACK snapshot digest
- model, graph, adapter, candidate, and strategy digests
- candidate priority and resolved tie-break outcome
- one-to-one role-to-Provider map
- feasibility evidence and rejected-candidate reasons
- artifact preparation mode

Validation: every role appears once; every Provider appears once; the candidate
belongs to the certified catalogue; all constraints derive from the closed
snapshot. Candidate feasibility uses that candidate's own roles and edges; list
order and any legacy global role hint cannot change the result.

## TerminalEvidenceRecord

One candidate-bound, schema-versioned record for a protocol, result, runtime,
redaction, child-exit, or cleanup oracle.

- repository-relative evidence path and SHA-256 digest
- evidence schema/version and candidate/source/SIF identity
- the structured fields required by that oracle (lifecycle sequence and
  request counts, numerical/token result, runtime/backend and physical-device
  identity, child exit/signal/timeout records, or cleanup status)

The record is invalid when represented only by a `PASS` string, an unbound path,
or a digest without readable matching bytes. The final validator consumes these
records rather than trusting labels emitted by a launcher.

## RoleAssemblyAssignment

Signed instruction for one Provider to realize one complete role.

- request, attempt, generation/invocation, and deadline
- ACK snapshot, model, graph, adapter, candidate, and plan digests
- role and Provider identity
- role kind; range roles carry a non-empty layer interval, while
  `COMPONENT_SET` carries a non-empty canonical node set and the reserved
  `layer_begin=0, layer_end=0` empty-range sentinel
- canonical object names, ranges, byte counts, and digests
- input/output/dependency schemas
- runtime/backend requirements
- security domain, protection epoch, signature, authorization, and replay fields
- assignment-scoped input-fetch authorization digest when this is the declared
  input-ingress role

State: `RECEIVED -> VERIFIED -> MATERIALIZING -> READY -> EXECUTING -> TERMINAL`
or fail closed at the first invalid transition.

## AssembledRoleCacheEntry

- candidate/model/graph/adapter/assembler ABI/role-kind/role/runtime identity
- security domain and protection epoch
- verified canonical object digests
- local executable path or runtime handle
- device assignment and byte accounting
- creation/access time and bounded retention state

A hit is reusable only after the assignment identity and all digests match and
the current request is independently authorized. A stale or revoked protection
epoch invalidates the entry even when model and graph digests match.

## QualificationCandidate

The only identity allowed to cross the expensive boundary.

- source and dependency/toolchain digests
- native/Python runtime and SIF build recipe digests
- local test, MiniNDN, and exact-SIF replay harness digests
- submit bundle and rendered profile digests
- SIF digest after local construction
- YOLO/Qwen package and workload/oracle digests
- signed external Qwen3.6-27B model-manifest digest, when `QWEN-F` is present
- external artifact authority/key identity and manifest-signature digest, when
  `QWEN-F` is present; a profile-only or unsigned manifest is invalid
- invocation-input reference, security-policy/protection-epoch, and
  plaintext-redaction contract digests
- validation/evidence schema digest
- invalidation state and earliest restart gate

State:

```text
DRAFT -> DESIGN_CONVERGED -> LOCAL_QUALIFIED -> INPUTS_CLOSED
      -> SIF_QUALIFIED -> REMOTE_READY -> EXECUTED -> CLOSED
```

Any changed plane moves the candidate back to its contract-defined earliest
state; a prior candidate remains immutable evidence.

## WorkloadCase

- case ID and model family
- input fixture reference/digest and preprocessing identity; fixture bytes may
  exist only in the controlled process that executes the case and MUST never be
  copied into a plan, log, or evidence artifact
- input transport mode; inline ceiling or repository reference identity,
  encryption flag, authorization scope, and protection epoch
- capability profile and expected candidate/role map
- expected numerical or token oracle
- request count and cold/warm or continuation semantics
- timeout/stop conditions
- required runtime/device and terminal oracles

Registered cases: Y-A, Y-B, and Y-N locally for YOLO; Q-C and Q-W locally for
the frozen Qwen reference; a non-qualifying `Qwen-runtime-smoke` for exact-SIF
packaging; and YOLO-F/QWEN-F on Tiger. Y-N contains the fixed order-control and
six negative subcases from `contracts/yolo-minindn-runner-v1.md`, but remains
one inventory command and one aggregate case record. Its capability negative
must remove `FullModel` and at least one required shared-candidate role, so
neither registered candidate is feasible; removing only one shared role is not
a valid no-feasible-candidate setup.

## LocalSuiteRecord

One source-bound item in the registered local qualification inventory.

- suite or case identifier, exact binary/selector/entrypoint, and command digest
- candidate/source identity and effective configuration digest
- child PID, start/end timestamps, exit status, signal, timeout status, and
  cleanup result
- protocol/result oracle references and first failing layer, if any

The runner snapshots the complete inventory into a fresh evidence root and
records both its canonical digest and file digest. Q-C/Q-W use the maintained
Spec175 wrapper's M01/M11 success marker; a zero exit without the registered
marker is not a passing case.

An inventory item is not complete until its supervised child record is present;
an aggregate process result cannot satisfy or replace an item record.

## LifecycleEvidence

- candidate, job, process, request, attempt, plan, and model identities
- one JSONL record per required milestone, each with `caseId`, monotonic
  `sequence`, timestamp, milestone name, immutable coordinator-bound
  `requestId` and ACK attempt identity, and only non-secret digests/counts;
  milestone names are unique and strictly ordered. The live driver must bind
  both identities before the first event; provisional IDs require an explicit
  opt-out, are permitted only in isolated journal tests, and cannot qualify a
  case
- timestamped protocol milestones, including `INPUT_REFERENCE_PUBLISHED` before
  `REQUEST_SENT` for `REPO_REF` cases
- selected candidate and role map
- canonical fetch/verification/assembly and cache outcome
- device/runtime provider evidence
- dependency/event/token/Response lineage
- oracle comparison
- every child exit and cleanup result
- first failing layer or terminal verdict
- redaction proof that input/result plaintext and secret capabilities are absent

No single marker is sufficient; terminal acceptance requires all required
fields to agree.
