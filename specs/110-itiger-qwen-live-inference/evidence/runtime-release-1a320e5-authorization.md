# Runtime release authorization: accepted local OCI 1a320e5

**State**: `AUTHORIZED_NOT_STARTED`

**Authorization time**: 2026-07-14, after live Cisco SSO/Duo reconnection.

## Immutable candidate binding

| Field | Value |
|---|---|
| local image | `ndnsf-di:spec110-local-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` |
| local image ID | `sha256:57f3804cf787bec41d69f4dadcf286d030419b3cbd247678c92e0d2313829b6a` |
| local unpacked size | `8688519721` bytes |
| source revision | `1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` |
| source seal | `sha256:48b6832b59c128353f42c8c50027ba9c8b46a14beb8ee6c8a92c3d1095a4f58b` |
| source-seal file SHA-256 | `009f2915098fa3c861dbf3b129076cad1a29f12cfedba359612f7f79169a56f7` |
| Foundation source | `228341e0ea1f28956015fbaa30d2bf58a56b7789` |
| Foundation local image | `sha256:a07c4470ed89b1d7da8fd626f94c4dc9062a2125dc969242a4588d5d87cf7158` |
| release ID | `spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` |
| exact GHCR tag | `ghcr.io/matianxing1992/ndnsf-di:spec110-runtime-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` |

The exact GHCR tag was absent under an empty Docker configuration before
publication. Every earlier GitHub/local failure identity remains frozen. This
record supersedes only the failed release dependency of T047/T160; it does not
rewrite or retry any earlier build or job.

## Local acceptance carried into release

- complete GPU image built once in 1327 seconds;
- NFD/NFDC 24.07 and non-root container control path passed;
- 244/244 C++ targets, all Python wheels, and every Qwen entrypoint passed;
- `RUNTIME_LIBRARY_CLOSURE_PASS:217`;
- PyTorch `2.6.0+cu124`, ONNX Runtime `1.20.1`, and Transformers `4.48.2`;
- read-only root, model, and artifact mounts passed;
- no Qwen model weights, identities, or credentials are embedded;
- 107/107 offline tests, five container integrations, preflight, YAML, source
  secret scan, CodeGraph, GSD health, and strict structural audit passed.

## Live substrate snapshot

- VPN state: connected to `vpn.memphis.edu`;
- SSH identity: `tma1@itiger`;
- partition: `bigTiger`;
- selected probe GRES: `gpu:rtx_5000:1`;
- RTX 5000 nodes `itiger07` through `itiger11` were idle at discovery time;
- account association limit: 3 nodes and 24 GPUs;
- Apptainer: `1.3.4-1.el9`;
- durable project root: `/project/tma1/ndnsf-di`, observed usage 961 MiB;
- persistent encrypted GHCR credential helper: PASS.

These are mutable facts and must be captured again in the allocation evidence.

## Authorized exactly-once actions

The user's `VPN CONNECTED` response followed the explicit plan to publish this
candidate, materialize its SIF, and run one bounded GPU runtime probe. It is
recorded as authorization for only these actions:

1. retag and push the exact local image once, without rebuilding any layer;
2. record the immutable GHCR digest and prove anonymous digest access;
3. submit one bounded CPU Slurm job to materialize and verify `runtime.sif`;
4. submit one bounded RTX 5000 GPU runtime probe after SIF PASS;
5. preserve the first terminal failure and do not auto-rerun or substitute a
   different image, GPU type, time limit, or job identity.

This authorization does not include a Qwen model download, standalone Qwen
inference, NDNSF-DI distributed inference, multi-node execution, performance
measurement, production deployment, firewall change, persistent daemon, or
login-node compute.

## Stop conditions

Stop before the next boundary if publication is not digest-addressed and
anonymously readable, SIF checksum/static verification fails, Slurm allocation
does not match the frozen request, `apptainer exec --nv` cannot inject the
driver ABI, CUDA/ONNX falls back to CPU, evidence cannot be promoted durably, or
the exact submission outcome is unknown.
