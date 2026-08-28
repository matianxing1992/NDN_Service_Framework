# Spec 168 v38 Replacement Candidate Audit

## Verdict

**PASS Gate D; authorize one linked replacement Gate E campaign.** Campaign
v37 is closed and must never be retried. Job 182393 crossed the real Request
boundary on all three Providers, then closed with zero ACKs because the Python
to native adapter silently suppresses an ACK when the Python callback raises.

## Frozen replacement

- Campaign: `spec168-campaign-v3-cc950f1684c07ea491d9`.
- Source: `sha256:1e192ca033600a6c4b10ad1e2393172989d08ff129956aaf551712d5e06b1949`.
- Source bundle: `sha256:7703964b4813502835c1b39c8106dd0795596988e74af18ed992d82ba24b2a8c`.
- Gate C job/result: 182401,
  `sha256:860771ff5ef2cf7f9386587e3f05b7f0719ca46cde5d578ea5899140269f0d76`.
- Unchanged route/launch input:
  `sha256:04ca3bfb8137d813e5c3e8b0e36ce84c8b006f2373eeb6bc4a05e86fa970be3f`.
- Unchanged Gate E schedule input:
  `sha256:4ccadcd09aa4f715095285e9d39d28dc1bbefc6a5cad2ecacd633b147337d578`.
- Campaign manifest:
  `sha256:6629542f0dd61d1b10f80e7df15521aeb16baa04db4329e5f9a4819a73351335`.

## Repair boundary and evidence

- An ordinary exception from either Python ACK registration path now produces
  one bounded local `NDNSF_ACK_HANDLER_EXCEPTION` marker. The marker includes
  service name, exception type, and at most 240 whitespace-normalized message
  characters; it never logs handler arguments or decrypted request payload.
- The requester receives a fail-closed, non-suppressed negative ACK with the
  generic `INTERNAL_ERROR` reason. The exception cannot silently imitate packet
  loss, and no detailed exception text crosses the wire.
- Seventy-two focused contracts pass, including the new exception contract.
- The exact parent Docker image and production copy-up entrypoint execute the
  failing callback contract within 2 GiB memory / 2 GiB memory-plus-swap.
- Gate C job 182401 passed in 20 seconds on RTX 5000: all three stages used
  `cuda:0`, expected/actual top token were both 8065, and CPU fallback was false.
- Gate C jobs 182398 and 182399 are retained separately as pre-Apptainer
  submission-variable failures. Neither executed Python, CUDA, or the model and
  neither contributes to the candidate PASS.
- No model, planning, policy identity, route, analyzer, schedule, SIF, native
  ABI, or Qwen artifact changed.

## Resource and security boundary

The 8 GiB development host runs only non-materializing tests, exact Docker
smokes capped at 2 GiB, or real MiniNDN explicitly bounded to 6 GiB memory and
7 GiB memory-plus-swap. CUDA and full-model work remain on TigerCluster. The
v37 failure cleanup removed its ephemeral selection key and retained no secret
key or token files.

## Authorized action

Submit campaign `spec168-campaign-v3-cc950f1684c07ea491d9` once with request ID
`spec168-cc950f1684c07ea491d9-single`. This run is expected either to complete
the request or expose the exact sanitized ACK exception. Preserve its terminal
result and never auto-retry this identity.

## Five-tool gate report

- Context Mode: stable and active-Spec checks passed; repository files remain
  authority.
- CodeGraph: synchronized and used for the Python/native ACK failure trace.
- Spec Kit: source identity, immutable bindings, local contracts, exact-image
  smoke, Gate C, and Gate D admission pass.
- GSD: the persistent deployment-fidelity goal remains active.
- ARS: not applicable; this is implementation admission, not research.
