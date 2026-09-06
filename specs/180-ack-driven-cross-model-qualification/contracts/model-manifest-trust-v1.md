# External Model Manifest Trust Contract v1

This contract defines the trust boundary for model manifests supplied outside
the SIF. Its machine-readable authority registration is the `modelManifest`
entry in `contracts/trust-root-registry-v1.json`. It is distinct from the YOLO
candidate-catalogue authority: a valid YOLO catalogue signature does not
authorize a Qwen model artifact.

## Registered artifact authority

The repository-owned Spec180 trust-root registry MUST name the artifact
`authority_id`, `key_id`, public-key algorithm, signature algorithm, public-key
digest, manifest schema version, and accepted model families. The public key is
content-addressed and fixed by the registry; a profile, environment variable,
Provider ACK, or staged file MUST NOT replace it. The private signing key is an
export-time secret only and is never committed, embedded in the SIF, staged to
Tiger, or written to evidence.

## Signed fields

The signature covers a deterministic canonical serialization of:

- model family, model/revision identity, source revision, and graph digest;
- every canonical graph, stage, initializer, and auxiliary object name, size,
  and digest;
- tokenizer/chat-template identity and stop-policy identity when applicable;
- preprocessing/postprocessing identity and input/output schema;
- the manifest schema/revision and signer metadata.

Map order, whitespace, and encoding are normalized before signing and verifying.
Duplicate, unknown, missing, or digest-inconsistent fields fail closed.

## Verification boundary

The release and Tiger pre-dispatch gates MUST verify the manifest against this
registered authority before staging or workload entry. A missing, unsigned,
unknown-key, expired, or altered manifest fails before upload, staging,
submission, or model fetch. Qwen-F therefore requires a signed manifest for the
Qwen3.6-27B ONNX graph and all stage/tokenizer/stop objects; a local fixture or
packaging smoke cannot satisfy this gate.

The registry must have `status: CONFIGURED` before model-manifest
verification can run. A missing or unconfigured registry is an
implementation-phase blocker; T001 and every release gate must fail closed
until this entry is populated and its public-key digest is verified.

Evidence records only authority/key identity, public-key and manifest digests,
schema/revision, and verification status. It never records private keys or
model/input/result plaintext.
