# Data Model: Data-Centric Authorization Evaluation

## AuthorizationCase

Represents one independently executable allow/deny claim.

| Field | Meaning | Validation |
|---|---|---|
| `case_id` | Stable case identifier | Unique, immutable |
| `user_permission` | User service right state | `present` or `absent` |
| `provider_permission` | Provider service right state | `present` or `absent` |
| `policy_epoch` | Relation to current policy | `current` or `stale` |
| `signer_name_relation` | Name/signing identity relation | `valid` or `mismatch` |
| `user_token` | User token condition | `fresh`, `mismatch`, or `replay` |
| `provider_token` | Provider token condition | `fresh`, `missing`, `mismatch`, or `replay` |
| `expected_gate` | Gate expected to decide the case | Named security boundary |
| `expected_terminal` | Expected result | `allow` or `deny` |
| `expected_executions` | Expected handler count | Non-negative integer |

## AuthorizationObservation

The result of one case repetition.

| Field | Meaning |
|---|---|
| `case_id`, `repetition` | Join key to the registered case |
| `started_at`, `completed_at` | Run interval |
| `terminal_status` | `pass`, `fail`, `timeout`, or `invalid` |
| `observed_gate` | Gate that accepted or rejected the message |
| `handler_executions` | Service handler entry count |
| `responses_accepted` | User-accepted response count |
| `latency_us` | End-to-end observation when defined |
| `crypto_counters` | Key wrap/unwrap, symmetric operations, cache hits/misses, failures |
| `evidence_files` | Logs, traces, and summaries retained by manifest |

## OnboardingSubject

Captures the existing Provider and newly provisioned User across an onboarding
transition.

| Field | Meaning |
|---|---|
| `provider_binary_hash` | Provider executable identity |
| `provider_service_config_hash` | Local service configuration identity |
| `provider_identity_hash` | Provider certificate/key metadata identity |
| `provider_trust_config_hash` | Local trust policy identity |
| `policy_epoch_before`, `policy_epoch_after` | Epoch transition |
| `manual_provider_changes` | Count and description of operator edits |
| `automatic_refreshes` | Controller manifest/permission refresh operations |
| `control_bytes` | Refresh/onboarding control traffic |
| `first_invocation_status` | New User's first terminal result |
| `time_to_first_success_ms` | From authorized User readiness to accepted response |

## ExperimentManifest

Immutable evidence index validated by the schema in `contracts/`.

Required relationships:

- One manifest identifies one subject, configuration, exact command, and terminal status.
- Every observation belongs to one manifest and one registered case or scale point.
- Every evidence file has a relative path, byte size, and SHA-256 hash.
- Paper results reference canonical manifest IDs rather than mutable directories.

## ClaimRecord

| Field | Meaning |
|---|---|
| `claim_id` | Stable paper claim identifier |
| `wording` | Current bounded claim |
| `status` | `PLANNED`, `PARTIAL`, `SUPPORTED`, or `REJECTED` |
| `implementation_evidence` | Source invariant references |
| `prior_work_evidence` | Primary-source references |
| `experiment_evidence` | Canonical manifest/result references |
| `paper_locations` | Sections containing the claim |

State transition:

```text
PLANNED -> PARTIAL -> SUPPORTED
    |          |
    +--------> REJECTED
```

`SUPPORTED` requires every registered evidence class for that claim. `PARTIAL`
may appear in limitations or planned evaluation but cannot support a numerical or
superiority statement.
