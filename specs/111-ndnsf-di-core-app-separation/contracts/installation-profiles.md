# Contract: Installation Profiles

## Core distribution/profile

Contains DI Core contracts/execution bindings and required NDNSF dependencies.
It excludes APP façades, planner implementations, GUI, experiments, operations
implementations and optional ONNX/Qwen/llama implementations.

Builds as a separate installable artifact.

## Optimization SDK distribution/profile

Contains public immutable contracts, protocols, instance-scoped registry,
allowlisted loader and contract-test helpers. It depends on Core contracts, not
Core execution internals, APP implementation or model adapters, and builds as a
separate installable artifact suitable for third-party development.

## APP distribution/profile

Contains APP SDK and depends on Core. Planner/model adapters are selected
explicitly rather than imported by Core.

It owns deployment definition/revision resolution, the fixed RuntimeJournal,
durable deployment/request handles and reconciliation. Runtime state is written
only to an operator-configured mounted persistent root, never an immutable
package/image layer. The root may contain an owner-only protected request wire-
envelope spool; journal/status/metrics contain only its digest-bound reference,
never plaintext input.

Provider processes are launched by external Docker/Slurm/systemd/operator
adapters using the APP profile entrypoint. APPDeployment selects already-live
generic agents and manages revision/model lifecycle inside them; the APP package
does not become an infrastructure scheduler or process supervisor.

Builds as a separate installable artifact.

## Planner distribution/profile

Contains planner registry plus reference fixed/cost policies and depends only on
Core contracts, not Core execution internals.

## Model distributions/profiles

Each optional model profile installs one or more planner/runner adapters and
their optional dependencies. Missing profiles do not break Core import.

Selected model adapters build as separate installable artifacts.

## Operations distribution/profile

Contains `ndnsf-di` validate/resolve/plan/apply/status/wait/rollback/drain/delete/
doctor/events/metrics and request submit/status/wait/result/cancel/stream
adapters. It calls public APP owner interfaces, contains no lifecycle/inference
state machine and is absent from Core-only inventory.

Platform adapters may render and reconcile an immutable
`RuntimeAllocationHandoff` and a distinct `InfrastructureAllocationHandle`.
For iTiger, Docker/OCI is only the build/distribution source; the execution
adapter renders bounded Slurm work that runs the exact SIF with Apptainer
`--nv`. Platform adapters do not absorb APP deployment state, request state,
optimization policy or Core execution semantics.

## Compatibility profile

Temporary aggregate matching the old install/import/command experience. It is a
mapping layer only and has a bounded exit gate.

## Distribution and namespace ownership

| Distribution | Exclusive ownership |
| --- | --- |
| `ndnsf-di-core` | `ndnsf_distributed_inference/core/**` and installed native Core SDK files |
| `ndnsf-di-sdk` | `ndnsf_distributed_inference/sdk/**` |
| `ndnsf-di-app` | `ndnsf_distributed_inference/app_sdk/**` |
| `ndnsf-di-planner` | `ndnsf_distributed_inference/planner/**` |
| `ndnsf-di-adapter-*` | matching `adapters/<name>/**` and native adapter target |
| `ndnsf-di-ops` | `ndnsf_distributed_inference/ops/**` and canonical operations entry points |
| `ndnsf-distributed-inference` | root `__init__.py`, legacy module delegations and legacy entry-point delegations only |

Owner wheels use a shared PEP 420-compatible namespace and never ship the root
`__init__.py`. Installing the compatibility wheel converts the root into the
legacy façade while leaving owner subpackages intact. Wheel `RECORD` file sets
must be disjoint, and clean uninstall tests prove one distribution cannot delete
another distribution's files.

Validation:

- isolated build/install inventory per profile;
- clean wheel install for Core, SDK, APP/planner and selected model adapters;
- standalone external optimizer wheel install with no repository path injection;
- out-of-tree native runner compile/link against installed public headers;
- Core-only import module snapshot;
- negative optional-dependency tests;
- compatibility command/import tests;
- static OCI profile/Dockerfile/SBOM-plan ownership checks; actual image-layer
  and generated-SBOM validation is deferred to Spec 110.
- wheel `RECORD` collision and install/uninstall-order checks.
