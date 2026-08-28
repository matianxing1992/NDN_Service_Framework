# Spec 168 v39 Replacement Candidate Audit

## Verdict

**PASS Gate D; authorize one linked replacement Gate E campaign.** Campaign
v38 is closed and must never be retried. Job 182411 proved the ACK observability
repair and isolated one deployment-contract defect shared by all Providers.

## Frozen replacement

- Campaign: `spec168-campaign-v3-0216204cf0b9bacae920`.
- Source: `sha256:5122149d43b2a918ac9188c506857a4d74bdcde6032fc3091824f95d1f13d5e7`.
- Source bundle: `sha256:576bd17903c8075219a5f51426a1c7663cfd4628f668e434932f0d0b9b5714ad`.
- Gate C job/result: 182412,
  `sha256:c2fba5b65e9e0138d08fddb3c9dc733974416d5dd520d20987a4a7a694729f67`.
- Unchanged route/launch input:
  `sha256:04ca3bfb8137d813e5c3e8b0e36ce84c8b006f2373eeb6bc4a05e86fa970be3f`.
- Unchanged Gate E schedule input:
  `sha256:4ccadcd09aa4f715095285e9d39d28dc1bbefc6a5cad2ecacd633b147337d578`.
- Campaign manifest:
  `sha256:b1c9310badae4504483b5ad14206b7a3846a7f6b8369989993b9ce6d24c20941`.

## Repair boundary and evidence

- Each generated residency record now contains the automatic planner's
  canonical `candidateDigest` as `partition_digest`, plus the exact `adapterId`
  and `adapterVersion`.
- Provider startup constructs and validates the complete durable residency
  identity before publishing its ready marker. Missing or malformed deployment
  metadata therefore fails before a user Request rather than inside async ACK.
- Seventy-three focused contracts pass. The exact parent Docker image built the
  real Qwen3-0.6B automatic-planning manifest and constructed the residency
  identity within 2 GiB, without materializing model weights.
- Gate C job 182412 passed in 1:47 on RTX 5000: all three stages used `cuda:0`,
  top token 8065 matched, and no CPU fallback occurred.
- No model, planning algorithm, policy identity, route, analyzer, schedule,
  SIF, native ABI, or Qwen artifact changed.

## Resource and security boundary

The 8 GiB host remains limited to non-materializing tests, 2 GiB exact Docker
smokes, or explicitly bounded MiniNDN. CUDA and full-model work remain on
TigerCluster. Job 182411 removed its ephemeral selection key and retained no
secret key/token files.

## Authorized action

Submit campaign `spec168-campaign-v3-0216204cf0b9bacae920` once with request ID
`spec168-0216204cf0b9bacae920-single`. Preserve its terminal result and never
auto-retry this identity.

## Five-tool gate report

- Context Mode: stable and active-Spec checks passed; repository files remain
  authority.
- CodeGraph: synchronized and used for the residency identity trace.
- Spec Kit: source identity, immutable bindings, local contracts, exact-image
  smoke, Gate C, and Gate D admission pass.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable; this is implementation admission, not research.
