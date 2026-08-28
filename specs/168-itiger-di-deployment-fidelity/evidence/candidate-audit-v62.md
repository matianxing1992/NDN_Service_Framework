# Spec 168 v62 Candidate Audit

## Verdict

**PASS Gate D; authorize exactly one Gate E three-node Qwen3-0.6B control.**
The admitted campaign is `spec168-campaign-v3-f3c87c4005a001189547` and the
only authorized request ID is `spec168-f3c87c4005a001189547-single`.

## Frozen identities

- Source identity: `sha256:d9815ff11485b4395afa9eee3cb29253e0f6524a9c7b7b1c77d3817ae7de9b6a`.
- Source bundle: `sha256:0134b5d0f47b3de1cbf84ab5b256408392e5532a2a873368f9b4df9cf683360a` (168 files).
- Native closure: core `1d376ff8...`, NDNSF extension `94a52caa...`, Repo extension `3121dc52...`, runtime ndn-cxx `83a80167...`, runtime ndn-svs `238796e3...`.
- SIF: `sha256:1f616fa773df4f8d821339cb7b0a3d62332b9f2197055bdb4e9f3dd081518368`.
- Qwen3-0.6B stage manifest: `sha256:8d8475db33efab5398014d0aac9570cda90bae5d50fd0128bed7b0215d0156f5`.
- Gate B manifest: `sha256:2147eb4229dad8594af38f8e3405177f904282077c7f4d2f62ab12cf1eccc861`.
- Gate C result: job 182489, `sha256:5dd8f723f87850ecef00590647e84a319383130597bd8b3a60abca7c1ec6250c`.
- Campaign manifest: `sha256:b72bea73643467b48ad0ca8a133f9fd54385ebdab2d96efc01d091ad2dd129a0`.
- Gate E wrapper: `sha256:0be7472e782a9b2faa4ddfdb8e23ae720ecf0a0a768b3f3120d724fa6985e198`.

## Admission evidence

- Gate A: 79 focused Python contracts pass: data-driven Selection (24),
  progress deadlines (6), multi-token generation (16), Provider truthfulness
  (10), real-MiniNDN gate (11), exact-SIF preflight (6), and remote analyzer
  contract (6). The earlier seven-case C++ provider-projection regression also
  remains retained.
- Gate B: v62 passed in 96.24 seconds under real MiniNDN. One durable Request
  produced four tokens and one authenticated Response across three data-driven
  roles. Its 20-event lifecycle runs from `REQUEST_CREATED` through
  `RESPONSE_PUBLISHED`; Repository unique bytes were 78,254,966.
- Gate C: job 182489 completed in 20 seconds on one RTX 5000. All three pinned
  Qwen stages used CUDA, CPU fallback was false, and top-token comparison
  matched. Job 182488 failed before Apptainer because upload mode normalization
  violated `source-modes.json`; it is retained and excluded from PASS evidence.
- Gate D remote checks: source bytes and modes, campaign schema, job shell,
  transitive helper paths, SIF/model manifests, artifact checksum set, clean
  campaign destinations, `/project` capacity, `/scratch` capacity, and at
  least three RTX 5000 nodes all passed. No competing user job was queued.

## Root cause closed by this candidate

The recurrent local/container disagreement was caused by a host-built NDNSF
core loading different same-SONAME ndn-cxx and ndn-svs binaries inside the
runtime image. Import-only smoke checks could not detect the resulting C++
ownership corruption. v62 binds a complete image-native core/NDNSF/Repo
closure plus the runtime ndn-cxx/ndn-svs digests. Real `ServiceProvider`
construction and the complete Gate B lifecycle now pass.

The operational prevention rule is therefore candidate promotion, not an
assumption that MiniNDN alone proves deployability: focused contracts -> real
MiniNDN -> remote mode-aware source verification -> exact SIF/CUDA -> immutable
candidate audit -> one remote campaign. Every layer consumes the same frozen
source identity and content-addressed assets.

## Authorized action

Submit `/project/tma1/ndnsf-di/jobs/spec168/v62-admission-container-path/gate-e-v62-001.sbatch`
once with comment `spec168:spec168-v62-gate-e-001`. Preserve the terminal Slurm
state, raw logs, routes, GPU inventory, full answer, token/stage trace, security
scrub, and analyzer result. Never auto-retry this campaign identity.

## Five-tool gate report

- Context Mode: stable and active-Spec health passed earlier; repository files
  remain checkpoint authority.
- CodeGraph: synchronized and used for Selection/Provider behavior.
- Spec Kit: Spec 168 controls identities, Gates A-D, and campaign admission.
- GSD: the deployment-fidelity goal remains active and resumable.
- ARS: not applicable to this implementation admission run.
