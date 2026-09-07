# YOLO input validation and manifest ownership correction

Date: 2026-09-07. Status: INPUT_CHECKS_PASS / RUNTIME_NOT_RUN.

The local package is
`Experiments/TigerCluster/.cache/model/spec180-public/models/canonical-package`.
The registry is in the sibling `contracts/trust-root-registry-v1.json`.
No key was generated or replaced, no package was edited, and no model was run.

The actual source owners were invoked from this checkout:
`adapters.yolo.candidates.verify_catalogue_signature`,
`verify_catalogue`, and `adapters.yolo.reference.load_reference`.
The reference reader used the repository fixture and input_size=640.

| Check | Observed result |
| --- | --- |
| Catalogue Ed25519 signature and registered public-key hash | VERIFIED |
| Catalogue body digest | sha256:3c12f7c8192c6a6283f917d02cb1d1fdd0454dec13059eee5c7ce7b88899e96a |
| Four-role catalogue ID | shared-backbone-two-shard-v1 |
| Four-role catalogue digest | sha256:3fd5fb9d9c46bd46240cf6cca132b940659ffccfffb634101c79139acd5dc891 |
| Graph: 158302 bytes | sha256:956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a |
| Weights: 9635728 bytes | sha256:1a998d3d56c0103e57ea6df557370a219a3df53380572b4e9337ff26b4a94a7f |
| Package manifest | sha256:9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d |
| Fixture | sha256:7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c |
| Oracle | sha256:ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175 |
| Reference shapes | input [1,3,640,640], output [1,50,6] |

Graph and initializer bytes were independently hashed against the package
manifest. The reference reader validated the fixture hash, preprocessing,
output dtype/shape, canonical row ordering and oracle hash. This verifies
retained inputs, not fresh independent inference or native graph parsing.

## Correct ownership

`build_yolo26n_adapter` verifies the catalogue signature and package digests.
`YoloCanonicalArtifactBinding.ensure` constructs
`ndnsf-di-canonical-model-manifest-v1` from the actual graph/initializer
publication references, hashes it, and publishes it through the encrypted
publisher. `apps/yolo.py::prepare_in_container` calls the adapter and the
existing runtime publication owner.

The old external `spec180-yolo-model-manifest-v1` summary identifies the
atomic catalogue candidate. It is neither this runtime canonical manifest
nor the signed catalogue. Its absence of a detached modelManifest signature
does not establish that the normal YOLO runtime requires a replacement signed
external summary. Do not fabricate one or weaken catalogue verification.
The dispatch-plane modelManifest field still needs explicit ownership and
binding before T002/T004 close; this report does not remove that requirement.

## Actual remaining boundary

Importing the complete adapter failed before construction because the host
Python wrapper cannot import `ndnsf._ndnsf`. The catalogue and reference
owners import independently and passed without replacing native bindings
with doubles. Full adapter/candidate enumeration, locked host native closure,
runtime root publication, source lock alignment, independent inference and
all network/GPU gates remain unverified.

This supersedes earlier blanket WAITING_EXTERNAL_INPUT statements about the
package/catalogue/oracle. It does not qualify the historical base SIF or
close T007–T017.
