# Runtime release publication evidence: 1a320e5

## Verdict

`PASS` — the exact locally accepted image was retagged and pushed once without
rebuilding. The resulting OCI manifest is anonymously readable by immutable
digest, so the bounded iTiger CPU SIF materialization gate is unlocked.

## Immutable bindings

- Source revision: `1a320e5d9e42f4f76e78aac62d9bb647e3b159f0`
- Source seal digest: `sha256:48b6832b59c128353f42c8c50027ba9c8b46a14beb8ee6c8a92c3d1095a4f58b`
- Foundation source revision: `228341e0ea1f28956015fbaa30d2bf58a56b7789`
- Accepted local image ID: `sha256:57f3804cf787bec41d69f4dadcf286d030419b3cbd247678c92e0d2313829b6a`
- Published tag: `ghcr.io/matianxing1992/ndnsf-di:spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0`
- Immutable OCI reference: `ghcr.io/matianxing1992/ndnsf-di@sha256:dddb41e5c89cc8f24fe1cdba250c0dd675f244a9f0cdd64a99bc34d48cd4cf2e`

## Exactly-once publication

- The remote tag was absent before publication.
- The accepted local image was retagged; no Docker build was run.
- Exactly one `docker push` was issued for this release identity.
- Push exit status: `0`.
- Push elapsed time: `8:51.65`.
- Published manifest digest: `sha256:dddb41e5c89cc8f24fe1cdba250c0dd675f244a9f0cdd64a99bc34d48cd4cf2e`.
- A clean, empty Docker configuration fetched the immutable manifest with exit
  status `0`; anonymous digest access is `PASS`.

## Supply-chain evidence boundary

- The source secret scan passed with `findingCount=0` across 107 files and
  378702 bytes.
- Authenticated and anonymous manifest JSON are byte-identical.
- Checksums cover the publication input, local image inspection, push log,
  immutable reference, manifests, access proof, scan, and tool inventory.
- `syft`, `cosign`, and `crane` were unavailable.
- This Docker CLI does not provide `docker sbom` or `docker scout sbom`; unknown
  subcommands misleadingly return the generic Docker help with exit status 0.
- Therefore this candidate makes no SBOM, signature, or provenance claim. This
  is the explicit unavailable-tool outcome allowed by T213, not a silent skip.

## Evidence locations

- Tracked authorization record:
  `specs/110-itiger-qwen-live-inference/evidence/runtime-release-1a320e5-authorization.md`
- Local publication evidence:
  `results/spec110-itiger-qwen-live/runtime-release/spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0/`

## Gate transition

T213 is complete. T214 may submit exactly one bounded CPU-only Slurm job to
materialize this immutable OCI digest as a SIF. This record does not authorize
Qwen inference and does not claim GPU runtime success.
