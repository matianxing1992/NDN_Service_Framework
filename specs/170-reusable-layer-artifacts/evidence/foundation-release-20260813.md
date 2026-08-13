# Spec170 foundation release — 2026-08-13

The immutable foundation build and keyless signature verification passed. This
is a foundation-layer release only; it is not yet a GPU image, SIF, frozen
candidate, or TigerCluster pass.

- Workflow run: `31691149296`
- Run URL: `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/31691149296`
- Workflow source revision: `996e6d7d197c498348fe9c715e6c13d49972e72e`
- Foundation source lock: `ndn-svs@060811333de68b9674e45522222a14d4e047bf28`
- Image: `ghcr.io/matianxing1992/ndnsf-di-spec170-foundation@sha256:94e0caed7c5675469843fc744a71f6dfd484d59594eb32b042b9288a75d7f15d`
- Image manifest: `sha256:633172e6033361430e6291f7a68d29a3bfcb6de1bf1e0d6c3a7134fb7a04e3b2`
- Dependency-lock digest: `sha256:710953c746bbae727f4a33ab0d6a7fb3a3810ce9956f5d71dbed9f131bc72d3e`
- Build result: native foundation compilation and GHCR push passed
- Signature: cosign keyless verification passed
- Signature issuer: `https://token.actions.githubusercontent.com`
- Signature subject: `https://github.com/matianxing1992/NDN_Service_Framework/.github/workflows/ndnsf-di-spec170-foundation.yml@refs/heads/Experimental`
- Artifact checksum: `b43c17a1dc64a83d7b20198cf1b96d83034c0df7d99729265060ef5159c056a9`

The image digest is now eligible as the immutable foundation input to the
Spec170 GPU image workflow. SIF materialization and all Gate C/D evidence are
still pending.
