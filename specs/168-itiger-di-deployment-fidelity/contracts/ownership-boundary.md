# Component Ownership Boundary

| Concern | Owner | Lower-layer support allowed |
|---|---|---|
| Generic Request/ACK/Selection/Response | NDNSF | unified service, request ID, security and transport |
| ACK closure and generic committed roles/dependencies | NDNSF Collaboration API | no model-specific policy |
| Model/adapter identity and dependency graph | NDNSF-DI | opaque payload carried by NDNSF |
| Post-ACK partition and placement strategy | NDNSF-DI | generic ACK metadata transport |
| Artifact identity, manifest and segmented fetch | DistributedRepo | NDNSF names/Data/security primitives |
| Disk/RAM/GPU cache compatibility | NDNSF-DI | Repo durable bytes only |
| Adapter load/warmup/device validation | NDNSF-DI | none in generic NDNSF |
| Stage dependencies and data-driven execution | NDNSF-DI | generic collaboration data publish/wait |
| Token loop and complete answer | NDNSF-DI adapter/runtime | generic terminal Response |
| Permissions, NAC-ABE and one-time tokens | NDNSF | DI must not bypass |
| Cluster scheduling, SIF and GPU allocation | experiment/operations | not runtime semantics |

## Change admission rule

Start diagnosis in the evidence boundary that failed. A patch may enter generic
NDNSF only when a minimal non-inference collaboration regression reproduces the
same defect. A patch may enter DistributedRepo only when an artifact-only
publication/fetch regression reproduces it. Otherwise the repair stays in
NDNSF-DI.

## Forbidden boundary leaks

- Generic NDNSF must not parse Qwen layers, ONNX graph nodes, KV-cache layouts,
  token IDs, GPU tensors, or model residency semantics.
- DistributedRepo must not choose Providers, model partitions, roles, or stages.
- Experiment scripts must not implement hidden runtime state transitions, inject
  fixed preparation waits, copy payloads through shared storage, or override
  production security.
- Model adapters must not generate new request IDs or silently change committed
  plans.

## Compatibility path

The legacy/preplanned collaboration path may accept explicit roles and
dependencies for tests or fixed deployments. Spec 168's default dynamic path
uses `begin_collaboration -> ACK_CLOSED -> commit_plan`, with all
inference-specific planning supplied by NDNSF-DI.
