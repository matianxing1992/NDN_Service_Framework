# Defect lineage: job 182413 Selection wire budget

| Field | Value |
|---|---|
| Original identity | `spec168-campaign-v3-0216204cf0b9bacae920`, Slurm `182413` |
| Boundary | final Selection transport/fanout |
| Symptom | only Stage 0 received the 7,468-byte-ciphertext compact LLM Selection; Stages 1/2 received smaller Repo Selections but not the LLM Selection |
| Narrow owner | generic NDNSF collaboration Selection projection and targeted prefetch naming |
| Security invariant | each projection retains Provider token proof, hybrid confidentiality/integrity, exact Provider binding, request ID, plan and attempt; no plaintext or fallback authorization |
| Repair | provider-specific V2 Selection projection per selected collaboration participant; compact multi-select retained only for non-collaboration calls |
| Local regression | `Spec168ThreeNodeRemoteGateTest.test_collaboration_selection_uses_bounded_provider_projections`; real MiniNDN/exact-container gate pending |
| Replacement identity | pending Gates A-D; must not reuse job 182413 identity |

This fanout is one logical `commit_plan`/Selection transition. It is not a new
Request, a reopened ACK window, or attempt 2.
