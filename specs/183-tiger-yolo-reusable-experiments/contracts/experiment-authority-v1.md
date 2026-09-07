# Spec183 Experiment Authority Contract v1

**2026-09-07 fix decision**: Spec180's own authority private keys are no
longer present on this machine (they were intentionally kept outside Git at
`~/.config/ndnsf/spec180/*.key` per catalogue-trust-root-v1.md and are gone).
Spec183 does not wait for an external owner re-issue. Instead it owns one
fixed experiment-only key set, created once by
`Experiments/TigerCluster/tools/spec183_authority.py issue`, and reuses the
same keys for every run and every provider. Regenerating the set would break
every signature and staged identity recorded against it, so the set is
treated as immutable for the lifetime of the experiment; the trust-root
registry is the frozen reference.

This inherits the Spec180 revision-112 functional-profile rule: Spec183
validates signature and trust binding for its experiments, not production PKI
administration. Passing this profile makes no production trust-root claim.

## Fixed key set (never committed)

Private material lives only under `Experiments/TigerCluster/.keys/`
(mode-0600 key files, mode-0700 directory, covered by
`Experiments/TigerCluster/.gitignore` `/.keys/`):

- `catalogue-authority.key` — signs the candidate catalogue document.
- `model-manifest-authority.key` — signs the candidate model manifest.
- `artifact-policy-authority.key` — the artifact-policy authority that the
  offline issuer and the protected-grant path bind through
  `protectionEpochs`; the dispatch profile's `security.protectionEpoch` must
  be one of `spec183-yolo-protected-v1`. Registered as
  `contracts/artifact-policy-authority.pub` in the trust-root registry
  (`grantSchema: ndnsf-di-key-grant-v1`). Registered 2026-09-07 as an
  additive owner; catalogue/modelManifest key IDs are unchanged.
- `offers/{BackboneNeck,DetectShard0,DetectShard1,Merge}.key` — fixed
  per-role provider offer identities, one per role, reused by every run.

The private keys are never committed, copied into a SIF, placed in a profile,
or sent to a Provider. Loss or compromise ends the experiment's signing
capability: any new key would invalidate the frozen registry and every
already recorded signature, so re-issue must be recorded as an identity
change in this feature's tasks.md before `issue --force` is used. Ordinary
export and runtime paths must never create or replace a key.

## Committed public identities

`specs/183-tiger-yolo-reusable-experiments/contracts/` holds the public side,
registered in `trust-root-registry-v1.json` (`schemaVersion` 1,
`status: CONFIGURED`):

- `catalogue-authority.pub`, `model-manifest-authority.pub`,
  `artifact-policy-authority.pub` — PEM public identities of the three
  signing authorities, with `keyId`, `publicKeySha256`, `manifestSchema` and
  `acceptedModelFamilies` bound in the registry (the artifact-policy entry
  binds `grantSchema` and `protectionEpochs` instead of `manifestSchema`).
  `publicKeyPath` is resolved relative to the Spec183 feature directory (the
  registry's own parent).
- `offers/{role}.pub` — PEM public identities for the fixed provider offer
  keys; `keyId` is the sha256 of the raw Ed25519 public bytes.

The registry must have `status: CONFIGURED` before any catalogue or model
manifest verification can run. A missing or unconfigured registry is an
implementation-phase blocker. `issue` is fail-closed idempotent: it never
overwrites an existing private key and refuses to rewrite an existing
registry whose key IDs differ, unless `--force` follows a recorded identity
change.

## Wire format

Identical to the format `scripts/spec180_contract_gate.py` verifies (shared
by Spec180 T004/T005/T012/T017):

- detached envelope on the manifest:
  `{"signature": {"keyId": ..., "algorithm": "ed25519", "valueB64": ...}}`
- signed payload = manifest minus `signature`, canonicalized as
  `json.dumps(payload, ensure_ascii=False, sort_keys=True,
  separators=(",", ":"))`
- verification binds keyId/algorithm and the public key to the checked-in
  registry; a manifest never carries its own authority, and a digest check of
  the registered public-key file precedes every verification.

## Runtime use

Verification runs in `Experiments/TigerCluster/tools/spec183_authority.py`
(mirroring the gate) before candidate staging and again wherever Spec183
T004/T005/T012/T017 consume an authority-signed manifest. A manifest signed
by an unknown key, with the wrong algorithm/family, missing its signature, or
failing the public-key digest is rejected before enumeration or staging.

## Evidence

Signed manifests and closures record `keyId`, public-key digest, signature
algorithm and schema in their provenance. Secret key material is excluded
from logs and evidence.
