# S1 Candidate Seal Evidence (Revision 112) — 2026-09-04

> **INVALIDATED — DO NOT REUSE.** Revision 123 changed the Controller
> PUBPARAMS readiness/runtime source after this candidate was sealed. The
> historical candidate identity and PASS observations remain for diagnosis,
> but this seal cannot be used for a new T014/T015/T016 route.

**Queue step**: S1 / T004+T006 — seal one current-role YOLO package and an
experiment-owned catalogue/offer trust set as one immutable candidate.

## What was done

The canonical package at
`.codex-tmp/spec180-yolo-candidate-current` (schema
`spec180-yolo26n-canonical-v1`, graph `canonical/yolo26n.onnx` + external
initializer `canonical/yolo26n.weights`, oracle
`oracle/full-model-output.npy`) was re-signed with the **registered
catalogue authority** whose public half is the checked-in
`contracts/catalogue-authority.pub` (file SHA-256
`sha256:9967848bb1efa16edd381007610573338f41d84a365d67ead6ac987136989e1e`,
registry keyId `spec180-yolo-catalogue-ed25519-20260903`). The private key
lives outside Git at `~/.config/ndnsf/spec180/catalogue-authority.key`
(mode 0600); only its public identity and digests are candidate inputs.

The previous package signature (made with an unregistered experimental
key) failed the checked-in `contracts/trust-root-registry-v1.json`; the
sealed registry file from the pre-revision-112 runs no longer exists.
Re-signing changed only the `catalogue.signature` block — both
`candidateDigest` values are byte-identical to the pre-sign values:

- `atomic-v1`:
  `sha256:35581dc38ed688a9c0e76c0fb173708361425ae1a64e86e64a38d8f23e1f61ef`
- `shared-backbone-two-shard-v1`:
  `sha256:3fd5fb9d9c46bd46240cf6cca132b940659ffccfffb634101c79139acd5dc891`

The current-role catalogue declares `BackboneNeck`, `DetectShard0`,
`DetectShard1`, `Merge` for the shared candidate and `FullModel` for the
atomic candidate (no `DetectHead0/1` legacy names).

## Verification evidence

- `build_yolo26n_adapter(.codex-tmp/spec180-yolo-candidate-current,
  registry_path=contracts/trust-root-registry-v1.json)` passes with
  `require_signature=True` (default): the catalogue signature verifies
  against the checked-in registry and public key.
- Focused disposition tests pass before this seal:
  `tests/python/test_spec180_yolo_ack_planning.py` (9 passed),
  `tests/python/test_spec180_provider_offer_trust.py` +
  `tests/python/test_spec180_ack_provenance.py` (15 passed).

## Sealed Y-A input bundle (fresh, revision 112)

`tools/ndnsf-di/prepare_spec180_yolo_case.py` produced
`.codex-tmp/spec180-yolo-y-a-inputs-r112/`:

| input | digest |
| --- | --- |
| packageManifest | `sha256:9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d` |
| catalogueRegistry (checked-in registry) | `sha256:095fd99c8e008b1e683eaf67bc26efe665bc6c27e93cc802888e8b5eacca5219` |
| config | fresh (superset of the pre-r112 config: current Repo service registry; inference block unchanged) |
| topology | `sha256:c849907bbfaa5230c1729e1a40e8b48657f083d545fa9f15a519143c93956601` |
| offerPrivateKeyMap | `sha256:928bffe96030874f9b834eacca7bebf2b7222e26ed1685156728f69d070336cb` |
| offerPrivateKeys[FullModel] | `sha256:ddcd0f9f812e41236a4931253b822764516b132b95dc166205cbef938c2563a0` |

The Provider-offer key `~/.local/state/ndnsf/spec180/provider-offer-keys/
FullModel.pem` was reused (byte-identical), keeping the private-key digest
bound in the earlier sealed records. The topology file is byte-identical to
the pre-r112 seal. The request-envelope key file is byte-identical
(`sha256:610b90e105e5bf1b116a559272d6fb594aa8418ae61f082c3d0753ebdd1ee318`);
a root-owned mode-0600 copy was made at
`.codex-tmp/spec180-yolo-y-a-secrets-r112/request-envelope.key` because the
runner requires the envelope key owner to match the MiniNDN process euid.

## Status

S1 seal inputs are `implemented` + `verified` (signature and adapter
verification are executed, not claimed). The live Y-A run against this
sealed bundle is the next evidence step (S2a) and is not claimed here.
