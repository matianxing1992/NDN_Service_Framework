# Spec 168 v36 Replacement Candidate Audit

## Verdict

**PASS Gate D; authorize one linked replacement Gate E campaign.** The closed
v35 campaign is not retried. Job 182389 proved that its generated policy kept
pre-existing Repository services under compute-Provider identities, so the
Controller issued four bootstrap identities while the launcher required seven.
The failure occurred before Repository, Provider, User, or Request execution.

## Frozen replacement

- Campaign: `spec168-campaign-v3-f7cab32e087fefc1e4b5`.
- Source: `sha256:9b936cf5bde4b1dd7f0d55cfd885050a8f9ce07d159ee5a5ea16fb7956e31ecb`.
- Source bundle: `sha256:c71ad8c936c5089862e467e89a28b628f45fe2027ccd20004c7722469a33c3b3`.
- Gate C job/result: 182390,
  `sha256:db717e2ca43318059b14e1d12c7f123c6654e4e82b0a8acee9c5a6ced78486c6`.
- Route/launch input:
  `sha256:21a6eb509f682e54fa6d2fd9eb4349ad83744968450e8c32322ad19db4b0090c`.
- Gate E schedule input:
  `sha256:4ccadcd09aa4f715095285e9d39d28dc1bbefc6a5cad2ecacd633b147337d578`.
- Campaign manifest:
  `sha256:7ae773bcc0a82b4d767c2c6c7169be07e7a4b78394f21917572eac1c84a91cd4`.

## Repair boundary and evidence

- `prepare-qwen36.py` now reconciles both new and already-existing Repository
  services to the three dedicated Repository identities. The operation remains
  idempotent.
- A focused regression generates the real policy shape and requires exactly
  three compute Providers, three Repository Providers, and one User.
- Gate E's unconditional EXIT trap removes keys, tokens, private keys, and
  `bootstrap-tokens.txt` before retaining either PASS or FAIL evidence.
- The source bundle closure and frozen execution modes pass. The exact parent
  Docker policy smoke passed under 2 GiB memory / 2 GiB swap.
- Seventy focused contracts pass locally. The retained Gate B real-MiniNDN
  result remains bounded by 6 GiB memory / 7 GiB memory-plus-swap; this v36
  change does not require another local model run.
- Gate C job 182390 passed in 1:55 on RTX 5000 with all three CUDA stages,
  expected/actual top token 8065, and zero CPU fallback.

## Resource boundary

The development host has 8 GiB RAM. Local work is restricted to bounded
MiniNDN within the 6 GiB / 7 GiB cgroup or non-materializing contract/state
tests. Full Qwen weights, CUDA, multi-node execution, and any workload without
a demonstrated bounded peak run only in TigerCluster Slurm allocations.

## Authorized action

Submit campaign `spec168-campaign-v3-f7cab32e087fefc1e4b5` once with request ID
`spec168-f7cab32e087fefc1e4b5-single`. Preserve its terminal result and never
retry this campaign identity automatically.

## Five-tool gate report

- Context Mode: stable and active-Spec checks passed; repository files remain
  authority.
- CodeGraph: used before implementation and synchronized.
- Spec Kit: prerequisites, frozen identities, source validation, and Gate D
  admission pass.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable; this is implementation admission, not a research claim.
