# v28 bounded Tiny Qwen3 MiniNDN result

## Identity and scope

- Candidate: `20260803T224500Z-v28-manifest-bound-tiny-qwen`
- Output: `results/spec168-real-minindn-dev/20260803T224500Z-tiny-qwen3-v28-oci`
- Image: `ndnsf-di:spec168-v24-native-a7ef9dc25ba5`, image ID
  `sha256:a383c5e001850e727cbf9a9a4a346432ca7883698adbc4f46ff5162997c5ddbb`
- Model: `NDNSF/TinyQwen3-Fixture`, revision `seed-168-config-v1`, model digest
  `sha256:33c921e7619dcd320ecab6191b1482fbcb6a9eb36fdff1ef06eaabe89e72e47b`
- Stage manifest:
  `sha256:1c2fec771854f2bc2a5ccc82a8d897ee3426335d3cf37cbb64a6d303b3588adf`
- Resource contract: 8 GiB host, 6 GiB container memory, 7 GiB memory+swap,
  CPU logic execution, no CUDA claim.

## Product-path evidence

- Real five-node MiniNDN topology; independent Controller, User, three Repo,
  and three Provider processes.
- One durable request ID:
  `/spec168%2Fminindn%2Ftiny-qwen3%2Fv28`.
- `ACK_CLOSED`, deferred artifact materialization, three final Selection role
  assignments, and the committed candidate digest
  `sha256:778a48e5d087bbc5fa74f3c376feeeefbdc7d15fbd127d82f5a21cf27bcd001d`.
- Repo registered 39,050,130, 154,175, and 39,050,661-byte immutable stage
  objects; Provider run directories did not contain copied source bundles.
- Stage 0 published hidden state, Stage 1 consumed and republished it, and
  Stage 2 consumed it and published a token for each epoch. There was no
  all-Provider-ready execution barrier.
- Final generated token IDs: `61787, 28911, 20514, 142589`; stop reason
  `MAX_NEW_TOKENS`; measured total 60,999.821 ms; campaign status `PASS`.
- No host NFD/NLSR remained after cleanup. Kernel records for the run interval
  contain no OOM, killed-process, or memory-cgroup event.

## Harness finding

The v28 launcher exited nonzero only after the completed campaign because its
post-run analyzer expected the single-pass `LLM_PIPELINE_QWEN_STAGE_OUTPUT`
marker for Stage 0. Full generation correctly emitted
`LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL`. It would next have incorrectly
required the eager-startup artifact marker even though deferred Selection
correctly emitted `LLM_PIPELINE_QWEN_SELECTION_PREPARE` for every stage.

Both classifier defects are fixed in
`tools/ndnsf-di/spec168_real_model_gate.py` and covered by
`tests/python/test_spec168_tiny_qwen_fixture.py` (3/3 PASS). The preserved v28
logs satisfy the corrected completion and readiness predicates for all three
stages. This is a product-path Gate B success with a measured analyzer
false-negative; it is not yet Gate C or TigerCluster admission.
