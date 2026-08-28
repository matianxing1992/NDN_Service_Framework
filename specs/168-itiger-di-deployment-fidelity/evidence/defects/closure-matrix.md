# Spec 168 defect closure matrix

This matrix is the closure index for T013.  Negative identities remain
immutable; a replacement is accepted only where the corresponding repaired
source identity and a later gate provide direct evidence.  A failure caused by
the host or an analyzer is retained as an environmental/evidence outcome, not
silently reclassified as a product pass.

| Boundary / original identity | Root cause and narrow owner | Repair and regression evidence | Replacement / closure |
|---|---|---|---|
| Compact multi-select did not reach all selected collaboration Providers; job 182413 (and formal-launch recurrence 182511) | Generic NDNSF Selection projection exceeded the transport budget and the launcher bypassed the native overlay. Owners: collaboration Selection projection and Tiger rank launcher. | Provider-specific V2 projections preserve token, request/plan/attempt and Provider binding. Mapped-native ABI/capability checks were added to the outer, SIF and child launchers. `test_collaboration_selection_uses_bounded_provider_projections` and mapped-native regressions pass. | Job 182518/v88: one authenticated Selection to all three roles, one Request, zero token Requests, complete response. **Closed.** |
| Model-free canary admitted model inputs; job 182512 | Canary mode still entered artifact hashing and one-prompt model-template validation. Owner: campaign admission split. | Canary/model-free inputs are separated in batch and rank wrappers; campaign contract tests pass. | Job 182513 is retained as the next negative and the corrected v88 control path passed. **Closed.** |
| Model-free ranks failed before Apptainer; job 182513 | Outer rank wrapper required `SPEC168_ARTIFACT_DIR` before the canary branch. Owner: rank-wrapper admission. | Artifact/model requirements are conditional on the selected mode; wrapper contract and local launch tests pass. | Job 182515 reached all three ranks and the v88 run passed. **Closed.** |
| Candidate native core was shadowed by the SIF core; job 182514 | Compatibility kernel prepended the old library path and spawned a child with mixed ABI. Owner: exact-SIF overlay/child process setup. | Post-kernel child proves the native core and extension hashes on every rank; `SPEC168_COMPAT_CHILD_NATIVE_ABI_PASS` is recorded in the remote rank trace. | Jobs 182515/182518 and Gate C v94 show the candidate ABI. **Closed.** |
| Completed control-plane response was rejected by the closure analyzer; job 182515 | Analyzer required a synthetic marker not emitted by the deployed runtime. Owner: evidence analyzer, not protocol runtime. | Analyzer accepts the real `LLM_PIPELINE_USER_RESPONSE` schema; untouched evidence was reanalyzed rather than rerun. | v88 canonical analysis is PASS. **Closed as evidence defect.** |
| Stage 1/2 used a cross-filesystem fallback while Repo registration was late; job 182516 | Assignment-bound registration visibility was not awaited and fallback hid the missing Repo object. Owner: DI registration wait and Provider fallback guard. | Atomic registration replacement, deadline-bounded progress and no pre-split fallback when DistributedRepo is configured. | Job 182518 proves all three role-specific Repo fetches with zero retransmits. **Closed.** |
| ACK snapshot closed before Collaboration ACK decryption; MiniNDN v16--v20 | `ackDecryptsInFlight` omitted deferred Collaboration callbacks. Owner: ServiceUser ACK lifecycle. | Shared `shouldTrackAckDecrypt(PendingCall)` predicate plus focused deferred-collaboration regression; Core/Collaboration suites pass. | v21+ and the v88/v92 remote admissions retain nonempty ACK/Selection evidence. **Closed.** |
| Repo and compute processes invalidated each other’s SVS sessions; MiniNDN v20 | Two processes reused one Provider identity. Owner: deployment identity/bootstrap setup. | Distinct Repo and compute identities, certificates, tokens and routes. | v21 onward, including v88, shows independent Repo ACKs and compute Selection. **Closed.** |
| Signed offer expired during request-first publication; MiniNDN v21 | Lease was shorter than the admitted preparation deadline. Owner: offer admission and Tiger launcher defaults. | Fail-closed lease validator and deadline-matched launcher settings; preparation/dependency tests pass. | v22 and v88 complete Repo publication/Selection. **Closed.** |
| Positive capability ACK preceded runtime-family proof; MiniNDN v22 | Qwen module import was deferred until after Selection. Owner: Provider capability preflight. | Explicit immutable model-family preflight before `PROVIDER_READY`; Qwen/Tiger contracts pass and Gate C verifies CUDA/Qwen identity. | v93/v94 large jobs pass the preflight and artifact gates. **Closed.** |
| V2 artifact identity entered the quarantined V1 execution-spec path; MiniNDN v23 | Generic Provider interpreted an opaque assigned artifact as a second execution specification. Owner: V2 adapter/runtime ownership. | V2 creates an adapter execution shell and binds the exact role/backend/device/digest through `runtime_preparer`; 9/9 Provider generation tests pass. | v26 and v88 reach role preparation without the duplicate fetch. **Closed.** |
| Assigned-device warmup required CUDA for CPU assignments; v24b | Qwen warmup ignored the committed device. Owner: Qwen adapter. | Warmup executes on the assigned device and rejects only a real mismatch; 5/5 device tests pass. | v26 CPU logic gate and v88 CUDA stages pass. **Closed.** |
| Dynamic dependency scope was replaced by the static startup graph; v25 | `DIRoleAssignmentV2` carried only a digest/scope names, so Provider built the wrong dependency view. Empty large-object publication also produced a false success marker. Owner: V2 assignment/dependency binding. | Assignment carries canonical role-local edge tuples and validates digest/scope/role uniqueness; empty publication fails closed. Selection/Dataflow, generation and planning suites pass. | v26 published and consumed the committed dynamic scopes; v88 completes the graph. **Closed.** |
| Local three-stage Qwen run exceeded the 6 GiB cgroup; v26 | Host capacity was insufficient for three simultaneous real Qwen runtimes. Owner: environment/capacity, not NDNSF protocol logic. | Tiny-Qwen local gate and exact-SIF CUDA Gate C are the bounded local checks; large weights are isolated to TigerCluster. | Retained as environmental boundary; not a logic defect and not rerun locally. |

The remaining large-model negative, job 182780/v93, is likewise an
environmental resource-boundary outcome: all source/SIF/artifact/ACK/Selection
and Stage-0 transfer gates passed, then rank 0 was killed by its 32 GiB Slurm
memory cgroup during Qwen3.6-27B preparation.  It is not a new NDNSF-DI logic
defect.  FR-019 admits exactly one new immutable resource profile, v94
(`96 GiB/node`, three RTX 5000 nodes); no second resource replacement is
permitted.

## T013 verdict

All encountered NDNSF-DI logic and harness defects have a named owner, a
minimal repair, focused regression coverage, and a later source/gate identity.
The two measured memory limits remain retained environmental outcomes. The
matrix is complete only when the admitted v94 large-model identity either
produces its required authenticated response or closes T015 as an explicit
resource/runtime failure; neither result may erase job 182780.
