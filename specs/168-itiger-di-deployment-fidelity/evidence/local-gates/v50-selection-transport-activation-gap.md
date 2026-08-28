# Spec 168 v50 Gate B - Selection transport activation gap

## Verdict

`BLOCK` — `EXEC_LOCAL_GATE_HARD_DEADLINE` after `900005.406 ms`. The
runner recorded `automaticRetry=false`; v50 was not retried or overwritten.

## Frozen identity

- Candidate: `20260804T074320Z-v50-checkpointed-selection`
- Source identity: `sha256:9f929c76f082673f48b4e2796fb26df89e7ae63ed1fbdde7f1b9073d2313083e`
- Gate command: `sha256:c8f9a90fa09aec34d6eb1999649eb0a113826727f9520c3716c5f62c05dd8e0b`
- Gate manifest: `sha256:22aeaa732ee03a77a9f41443782908611fdeea2c2b6c68903339b1d02bacec72`
- Gate checkpoint: `sha256:0c78a17806b90b042e60bd0588d6904821a49a16a9157478bc6d30bd8121de57`
- Launcher log: `sha256:f2dcb97b917fd8ec50df11e340026df8d7ec4e60157a40a250254e4da2c7c7e3`

## Narrowest lifecycle boundary

The Controller, three Repo Providers and three inference Providers started and
completed normal permission/NAC-ABE initialization. The inference Request
closed its ACK window and entered automatic planning. Deferred artifact
publication then opened one ordinary DistributedRepo `Artifact/v2/STORE`
collaboration. All three Repo Providers returned successful ACKs.

The User selected `/example/llm-pipeline/repo`, attached its `2196`-byte
provider-specific assignment, and published no subsequently accepted Repo
Selection. The three Repo Providers expired the matching pending request/token
state after approximately 300 seconds. No artifact publication, model fetch,
stage Selection, model load, generated token, or Response occurred.

Therefore v50 is a **Selection transport activation failure at deferred Repo
publication**, not a model, GPU, DistributedRepo throughput, or inference
failure.

Retained runtime log digests:

- User: `sha256:6b5a4bd7c6264526c325d18162e8c055d00cefc2f9d2bc769af5ea5351c5d1df`
- Repo 0: `sha256:994e8e0043baf3880b4a0d52b87668f5ad17f6934dbcdad1f32b8a46b289aa17`
- Repo 1: `sha256:d9832a3560c90538a8d63559a87f575cdd316ce9c2c007026d0b3aabfa0197f2`
- Repo 2: `sha256:632a38acd0255b761fb4f673916699030c32dacf411e91a7856e2049a8122ce9`

## Repair identity boundary

The native v49 image already implements authenticated provider-specific V2
Selection projection and exact-name targeted prefetch, but the LLM MiniNDN
harness never enabled the shared `NDNSF_SELECTION_TARGETED_PREFETCH` switch.
v51 enables it for `selection-dataflow-v2` and pins it into each node command
closure. This transports the same signed, hybrid-encrypted Selection and token
proof; it does not bypass authorization or create another collaboration
attempt.

Replacement candidate: `20260804T080022Z-v51-targeted-selection`. v51 must
pass focused tests and a new real MiniNDN Gate B before exact-SIF or remote
admission.
