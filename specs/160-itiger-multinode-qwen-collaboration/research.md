# Research: Existing Multi-Node Qwen Path

## Decision: reuse the three-stage Qwen layer pipeline

The repository already partitions Qwen2.5-0.5B's 24 decoder layers into three
stage packages. Stage 0 embeds and runs layers 0-7, Stage 1 runs layers 8-15,
and Stage 2 runs layers 16-23 plus normalization and the language-model head.
This is real layer-pipeline execution and is preferable to constructing a
prompt-chaining demo.

## Decision: use planned segmented dependency Data

`ProviderRuntimeContext.prefetch_input_large()` and
`publish_output_large_reference()` already connect provider roles through
planned NDN Data names. The underlying Core collaboration context publishes and
fetches segmented exact-name objects. No new transport protocol is required.

## Decision: require a pre-inference NFD allocation probe

The existing iTiger operating contract disables cross-node NDNSF-DI until an
allocation-scoped NFD TCP/UDP probe passes. The probe is therefore a hard
predecessor, not optional diagnostics.

## Decision: capability, not scaling experiment

One request can establish correctness and physical cross-node participation,
but it cannot establish speedup, capacity, reliability, or general scalability.
Those require a later preregistered repeated experiment.

## Alternatives rejected

- Two independent full-model replicas: no single request collaboration.
- Prompt output from one full model fed to another: application chaining, not
  Qwen layer partitioning.
- CPU ONNX stages: existing artifacts are useful references but do not satisfy
  the requested GPU-backed TigerCluster path.
- New tensor-parallel implementation: materially different architecture and
  unnecessary for the requested proof.
