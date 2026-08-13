# Spec170 GPU OCI release — 2026-08-13

This is the first accepted immutable GPU OCI candidate for Spec170. It is not
yet a TigerCluster or SIF execution result.

- Workflow: `31723000406`
- Source revision: `e23d759bb61159c8b3093e599fe301599d8c043f`
- Release ID: `spec170-runtime-e23d759bb61159c8b3093e599fe301599d8c043f`
- Foundation image:
  `ghcr.io/matianxing1992/ndnsf-di-spec170-foundation@sha256:94e0caed7c5675469843fc744a71f6dfd484d59594eb32b042b9288a75d7f15d`
- GPU image:
  `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916`
- Runtime record manifest digest:
  `sha256:b4b694048f8830a25eb6431f32adae0aa3b58f630ad58469022e6d8804be7886`
- SBOM digest:
  `sha256:efbe1dd65d2cc2389fdd779c6d08e4d9c1ddb0f5f4f190e3886d1a924469da63`
- Signature bundle digest:
  `sha256:50afed832dca3613d4bc7249657d8f1853e226064133ccf0000f55d55c2d6956`
- Actions artifact: `9191498909`

The workflow passed full native C++ assembly, Python lock verification,
runtime-library closure, static runtime import probe, cosign OIDC verification,
anonymous digest access, SBOM generation, release-manifest schema validation,
secret rescans, and evidence upload.

The candidate remains **OCI-PASS / SIF-PENDING**. No TigerCluster compute job
has been submitted because the current VPN session is disconnected; SIF
materialization must use a bounded Slurm CPU job after cluster access returns.
