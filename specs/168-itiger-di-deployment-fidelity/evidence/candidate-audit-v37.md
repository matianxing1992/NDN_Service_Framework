# Spec 168 v37 Replacement Candidate Audit

## Verdict

**PASS Gate D; authorize one linked replacement Gate E campaign.** Campaign
v36 is closed and never retried. Job 182391 proved its policy identity repair,
then failed before Request because a compatibility-kernel `PYTHONPATH` update
placed the installed SIF package ahead of the verified source overlay.

## Frozen replacement

- Campaign: `spec168-campaign-v3-ff47a90a70db779277be`.
- Source: `sha256:315c94fd0c75f8491c19e7f01f0811620a4992c9544f185bdec38bd5e6f2a6f4`.
- Source bundle: `sha256:20248e1b4de0397983f6fa4a19de9740fcfc917e578a423c4bc0fe3d337c898d`.
- Gate C job/result: 182392,
  `sha256:e7d04d380e0914ec6c4aa856aa4a7ddb493d1a275de8f7cd52dc85b6414c3781`.
- Route/launch input:
  `sha256:04ca3bfb8137d813e5c3e8b0e36ce84c8b006f2373eeb6bc4a05e86fa970be3f`.
- Gate E schedule input remains the unchanged v36 input:
  `sha256:4ccadcd09aa4f715095285e9d39d28dc1bbefc6a5cad2ecacd633b147337d578`.
- Campaign manifest:
  `sha256:3925ca92c4b8d958cbe8e3293df438dc395adc1b164bedb51d744e53e4642627`.

## Repair boundary and evidence

- The existing caller-supplied complete package overlay now remains ahead of
  `/opt/ndnsf-app/python` for the automatic-planning helper and all later
  Provider/User processes.
- A nonzero rank writes its rank-specific failure record and shared abort
  marker; every barrier observes the marker and exits promptly. The outer
  launcher provides the same propagation if failure occurs before the inner
  kernel starts.
- The exact parent Docker image executed the formerly failing automatic
  planning helper successfully under 2 GiB memory / 2 GiB swap.
- Seventy-one focused contracts pass. Source closure, source modes, JSON, and
  shell syntax pass.
- Gate C job 182392 passed in 1:35 on RTX 5000 with three CUDA stages,
  expected/actual top token 8065, and zero CPU fallback.
- No model, policy identity, route, analyzer, schedule, SIF, or Qwen artifact
  changed.

## Resource and security boundary

The 8 GiB development host runs only bounded MiniNDN under 6 GiB memory / 7 GiB
memory-plus-swap or non-materializing tests. CUDA and full-model work remain on
TigerCluster. Job 182391's post-cancel emergency scrub deleted three secret
files without reading them and verified zero retained key/token files.

## Authorized action

Submit campaign `spec168-campaign-v3-ff47a90a70db779277be` once with request ID
`spec168-ff47a90a70db779277be-single`. Preserve its terminal result and never
retry this campaign identity automatically.

## Five-tool gate report

- Context Mode: stable and active-Spec checks passed; repository files remain
  authority.
- CodeGraph: synchronized and used for the launcher/compatibility impact trace.
- Spec Kit: prerequisites, frozen identities, source validation, and Gate D
  admission pass.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable; this is implementation admission, not research.
