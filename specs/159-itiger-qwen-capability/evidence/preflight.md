# Preflight Evidence

**Captured**: 2026-07-27  
**Verdict**: PASS

## Access and cluster

- Cisco VPN: connected.
- SSH: `tma1@itiger`, UID 64102, group `RC-itiger-users`.
- Partition/account/QOS: `bigTiger` / `devs` / `normal`.
- Association limit: 120 CPUs, 24 GPUs, 3 nodes, maximum six running jobs.
- Cluster inventory: 8 H100 80 GB, 40 RTX 6000, and 40 RTX 5000 GPUs.
- Apptainer login version: 1.3.4.
- Project root: `/project/tma1/ndnsf-di`, observed usage 962 MB.
- Authoritative per-user project quota was unavailable; the 0.5B model and one
  approximately 9 GB SIF are far below observed filesystem availability.

## Candidate

- Local tag: `ndnsf-di:spec158-app-reuse-proof`.
- Local image ID:
  `sha256:8f8d2a0b219dc6ee0216c43a3ead9d65125850431f30cc26b7aa4b88c0f2f6e4`.
- Unique capability tag:
  `ghcr.io/matianxing1992/ndnsf-di:spec159-capability-8f8d2a0b219d-20260727a`.
- Immutable OCI digest:
  `sha256:a0e58822fe1275947fe060df85b555f63d69029ac70c1a6788aa4d77c2d8acd2`.
- Anonymous digest inspection: PASS.
- Authority: dirty-source development capability candidate, not formal release.

## Model

- Repository: `Qwen/Qwen2.5-0.5B-Instruct`.
- Revision: `7ae557604adf67be50417f59c2c2f167def9a775`.
- Project path:
  `/project/tma1/ndnsf-di/models/source/qwen25-0.5b/7ae557604adf67be50417f59c2c2f167def9a775`.
- Seal: 10/10 files exist with exact recorded size and SHA-256.
- Total sealed bytes: 999,604,126.
- Weight digest:
  `sha256:fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe`.

## First live submission identity

- Run: `spec159-run-sif-a0e58822fe127594-001`.
- Submission: `spec159-submission-sif-a0e58822fe127594-001`.
- Rendered script SHA-256:
  `d3c2739146979e57380e86d0094ea781318e64fe1e32d8f2f6226c931232dd4a`.
- Materializer SHA-256:
  `268e4b3328bd874f9537b82deb8ed5682bc3d19408b9dfd9a1cf3387adc3f5e1`.
- Placement: CPU-only, itiger07, 2 CPUs, 16 GiB, 30-minute hard limit.

Missing `/project` identities do not block OCI/SIF or standalone Qwen. They are
a hard prerequisite for the later secured NDNSF-DI gate and will be provisioned
or located without embedding secrets in the image or evidence.
