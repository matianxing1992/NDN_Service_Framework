# Specification Quality Checklist: NDNSF-DI Core/APP Separation

**Purpose**: Validate specification completeness and quality before planning
**Created**: 2026-07-14
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation choices are prescribed where an outcome-level contract is sufficient
- [x] Focused on integrator, external optimization-team, application-developer, maintainer, and evidence-consumer value
- [x] Written so ownership and migration outcomes are understandable without source details
- [x] All mandatory sections completed

## Requirement Completeness

- [x] No `[NEEDS CLARIFICATION]` markers remain
- [x] Requirements are testable and unambiguous
- [x] Success criteria are measurable
- [x] Success criteria describe observable outcomes rather than one mandatory implementation
- [x] All acceptance scenarios are defined
- [x] Concurrency, stale state, extension loading/trust/budgets/failures, package ownership, missing adapters, rollback, compatibility, and evidence edge cases are identified
- [x] Core-owned decision ports and SDK re-exports preserve dependency inversion with zero Core-to-SDK imports
- [x] APPDeployment/APPClient/APPProvider own extension execution and Core owns no plugin process or lifecycle
- [x] Every policy-bearing site maps to one of ten public Python policies; Runner creation and optional outcome observation are the only independent non-policy SPIs
- [x] Current internal policy algorithms become a versioned `DefaultOptimizationSuite` using the same ten public policies without privileged bypass
- [x] A process-local APP-owned `DistributedInferenceEngine` defines decision epochs, invalidation/re-entry, objective/snapshot lineage and evidence without becoming a network coordinator or authority
- [x] Model/target policies propose bounded authorized/compatible candidates; final selection is joint with plan/Provider feasibility and exact constraints become singleton
- [x] Objective metrics define units/direction/aggregation/normalization and predicted facts define confidence/horizon/source/freshness
- [x] Deployment lifecycle, scoped admission/scheduling, adapter-declared batching, typed tuning and phase/action cache decisions have explicit non-overlapping contracts and validators
- [x] One atomic execution intent prevents torn model/plan/assignment/target state and releases reservations on abort
- [x] The initiating requester identity is the per-attempt coordinator, while the authorized APPDeployment identity is the single writer for each deployment lifecycle stream
- [x] Existing generic leases, Targeted security, V2 request/Selection and attempt authority remain canonical; no second lease protocol or top-level coordinator name is introduced
- [x] Prepare and commit only reserve resources; exact authenticated Provider receipt closure and one immutable commit certificate are required before activation
- [x] Request attempt, Provider boot and deployment lifecycle epochs fence requester crashes, Provider restarts, partitions and competing writers
- [x] Network partitions fail closed for new authority and expose bounded cleanup rather than a false global-atomicity or availability guarantee
- [x] Periodic Provider cleanup reclaims leases, reservations, sessions, cache pins and runner handles without relying on later request traffic
- [x] Resource cleanup retains bounded attempt/lifecycle high-watermark tombstones long enough to reject delayed stale operations, and new prepare binds the expected Provider boot epoch
- [x] Fault injection covers every prepare/revalidate/commit/certificate/activate/result/release boundary plus requester crash and lifecycle conflicts before Engine implementation
- [x] One canonical deployment definition resolves to an immutable revision digest; metadata-only prepared sessions are not mislabeled as deployed runtime state
- [x] APPDeployment exposes validate, resolve, dry-run plan, apply, status, wait, rollback, drain and delete with idempotent durable operation handles
- [x] A fixed APP-owned RuntimeJournal preserves lifecycle, receipt, certificate, rendezvous and tombstone state across process restart without becoming an optimizer SPI, cluster database or consensus service
- [x] Journal locking, owner permissions, schema/version mismatch, torn writes, corruption and quota exhaustion fail closed with actionable diagnostics
- [x] Persistent state is namespaced by canonical application/NDN owner identity and deployment/request stream; traversal, symlink and cross-tenant record/spool reads fail closed
- [x] Model weights remain external digest-bound artifacts; secrets are references only and are excluded from revisions, journals, images and status output
- [x] Definition/revision state, running instance phase and reason-coded health conditions are distinct; READY and ACTIVE require fresh signed role/boot/artifact/adapter/permission/capacity evidence
- [x] Startup orders installation, persistent mounts, validation/doctor, externally supervised NFD/controller, generic externally launched Provider agents, APP-selected revision staging/activation and clients without a process-provisioning cycle
- [x] APPClient exposes durable submit/open/status/wait/result/cancel/stream handles; synchronous and Future paths are compatibility adapters rather than separate semantics
- [x] Durable submit persists only an authenticated/confidentiality-protected request wire envelope and journals its digest-bound reference; missing/expired/tampered input fails rather than being reconstructed
- [x] Upgrade, rollback, cancellation, drain and shutdown have explicit fencing and terminal-state semantics with no mixed-revision visible result
- [x] Operations CLI is a thin presentation adapter over the same APP APIs and stable status schema
- [x] A clean-profile MiniNDN validate-to-delete gate blocks operational-readiness claims
- [x] Docker/OCI is only the iTiger build/distribution source; exact SIF under Slurm/Apptainer is the runtime and no public always-on service is implied
- [x] Infrastructure allocation, APPDeployment and request handles/states are distinct and scheduler state grants no inference authority
- [x] The iTiger handoff binds new Spec 111 candidate/revision, OCI/SIF, model, process-map/network, identity/state and authorization digests
- [x] Generic allocation topology derives Provider roles from the revision, runs all project commands in the exact SIF and maps explicit GPU UUIDs
- [x] iTiger binds include read-only model/artifact/role identity, identity-partitioned persistent state and shared node-local NFD run path without broad writable project access
- [x] Single-node post-Spec-111 acceptance precedes selected-transport multi-node use and belongs to Spec 110 under a new candidate/authorization
- [x] Spec 111 uses local unit/native/package/static checks plus MiniNDN for every distributed acceptance and performs no OCI/SIF build, container-runtime invocation or iTiger/Slurm submission
- [x] Streaming progress/output/checkpoint facts bound recovery and prevent duplicate visible output
- [x] Outcome observation is off-path/idempotent/failure-isolated and stateful policies record state epoch/digest without a Core plugin database
- [x] Least-input and tenant/security cache scopes exclude prompt/tensor/credential/token/decrypted-policy disclosure by default
- [x] Placement/load balancing, parallel layout, scaling/residency, communication tuning, memory and backend selection are mapped to accepted owners rather than duplicated as extra policies
- [x] Ordinary Provider selection and multi-role placement share one assignment contract
- [x] Dispatch order and concurrency/window grants are atomic, while per-invocation execution tuning remains separate
- [x] Cache policy, recovery policy, execution-target policy and Runner adapter ownership do not duplicate assignment, partition, planner-registry or factory responsibilities
- [x] Static decision-inventory validation fails on unclassified or native-only optimization branches
- [x] Partial suites use evidenced named defaults while selected-hook failure has no implicit fallback
- [x] Worker-process containment and allowlisting are not represented as a sandbox for untrusted code
- [x] Namespace-package, wheel `RECORD`, install-order, and uninstall-order rules prevent cross-distribution file ownership collisions
- [x] Scope is clearly bounded
- [x] Dependencies and assumptions are identified

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
- [x] User scenarios cover Core use, standalone external optimization, concurrent assignment, and migration
- [x] Feature meets measurable outcomes defined in Success Criteria
- [x] Source/package examples clarify scope without constraining the specification to one implementation layout

## Notes

- The specification intentionally unifies ordinary and multi-role Provider
  assignment while separating application-owned objectives from Core-owned
  eligibility/application invariants.
- Spec 110 evidence remains immutable; any implementation of Spec 111 creates a
  new candidate identity before remote execution.
- External optimization is accepted only through public instance-scoped SDK
  contracts; Core never auto-discovers or imports third-party packages.
- The 2026-07-14 revised strict and semantic gates pass with 105 FRs, 59 SCs,
  4 user stories, 201 sequential tasks, 75 parallel markers, 10 policies and
  complete FR/SC traceability. The code-aware audit is PASS for design only;
  this is not implementation or experiment evidence.
