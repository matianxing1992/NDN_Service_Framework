# Data Model

This file is the detailed target data contract for Spec194. All records are immutable for one planning attempt. `unknown` is distinct from a measured zero.

## PlacementProfileCapability

Owned by the Provider's authenticated discovery/advertisement path; T002 locates and implements its exact current carrier. For each candidate Provider, retain the advertised supported placement profiles, provider identity, boot epoch, capture/expiry and authentication/source digest. The User fixes the eligible candidate roster, validates one current capability record for every member, then chooses a single request profile. A missing, stale or unauthenticated capability makes V4 coverage incomplete and selects the existing V3/default path for the whole request. Do not infer support from an observed offer or mix profiles within one request/attempt.

## SelectedPlacementProfile

Owned by the native User request planner. Records the chosen profile, frozen candidate-set digest, request/attempt identity, selection time and the validated capability source digests. This choice is immutable for that request/attempt; changing candidates or retrying with a new attempt requires a new decision and records. The profile selector may choose V4 only when every eligible candidate's current authenticated capability includes V4.

## NetworkObservation

Owned initially by Core telemetry collection; DI consumes a validated copy. Required fields:

| Field | Type / unit | Meaning |
|---|---|---|
| `src_peer`, `dst_peer` | canonical NDN identity | For `DATA_FETCH`, direction of the Data payload bytes: producer/source → consumer/receiver. For `ACK_RTT`, ordered requester/responder endpoints, explicitly marked as a round-trip interaction; never use the ACK direction to infer bulk goodput. Self-edge is not serialized as a network sample. |
| `direction_semantics` | enum | `PAYLOAD_SOURCE_TO_RECEIVER` for data-flow cost, `REQUESTER_RESPONDER_ROUND_TRIP` for ACK timing. Required to prevent reversing the Core consumer/producer lookup key into the wrong payload direction. |
| `kind` | enum | `ACK_RTT`, `RESPONSE_RTT`, `DATA_FETCH`; `ACTIVE_PROBE` is not enabled by V1. |
| `observer_peer`, `observer_boot_epoch` | identity/string | Process that measured the sample and its boot generation. |
| `dst_boot_epoch`, `topology_digest` | optional identity/digest | Must be present and match when the source can establish them; otherwise the observation is ineligible for optimized placement. |
| `captured_at_ms`, `expires_at_ms` | milliseconds since epoch | Observation interval bound; `expires_at_ms > captured_at_ms`; effective expiry is capped by request and offer expiry. |
| `sample_count` | unsigned integer | Number of successful samples represented by the summary. |
| `rtt_ms`, `first_byte_ms`, `elapsed_ms` | nullable milliseconds | Kept separate. `ACK_RTT` includes ACK responder processing; `DATA_FETCH.first_byte_ms/elapsed_ms` measures the data fetch path. |
| `wire_bytes`, `encoded_bytes`, `received_segments` | nullable unsigned integer | Exact sample counters. No estimate is substituted when absent. |
| `timeout_count`, `nack_count` | unsigned integer | Raw counts for the window; V1 does not invent a loss rate without a valid denominator. |
| `goodput_mbps` | nullable decimal | Derived only from actual wire bytes and elapsed time. |
| `source_digest` | SHA-256 digest | Commits only the allowlisted normalized sample fields; raw Data names, key scopes, and payload content are excluded before digesting and serialization. |

## ProviderResidencyView

Owned by the DI Provider/cache owner; carried by a signed Provider offer. Rows are restricted to model/graph/artifact candidates in the request.

Required identity: `provider`, `boot_epoch`, `process_epoch` where needed, `model_manifest_digest`, `graph_digest`, `artifact_digest`, `role`, `rank`, `backend`, `device_set`, and `protection_epoch`. State is one of `EXACT_LOADED`, `ASSEMBLED`, `CANONICAL_ONLY`, `KNOWN_MISS`, or `UNKNOWN`. Every state has capture/expiry and a provider signature through the containing offer. Cost facts may include exact `missing_verified_bytes`, `estimated_assembly_ms`, and `estimated_load_ms`; unavailable estimates are `null`, not zero. `EXACT_LOADED` also requires the existing runtime generation/fencing/topology checks. No plaintext, KV, grant key, raw request input, or unrelated cache digest is exposed.

## ProviderResourceView

Owned by the Provider readiness/resource collector and carried only for the current candidate roles. Rows report the real collector's resource dimensions, units, totals and available amounts; T002 freezes the exact field map against current native readiness structs while implementing the V4 offer. Resource and worker-capacity state is `OBSERVED`, `KNOWN_EMPTY`, or `UNKNOWN`: unknown uses `null`, a known empty set uses `[]`, and zero is reserved for a measured numerical zero. Every row binds to provider boot/process epoch, sample time, expiry and request candidate-set digest. Do not send unrelated host inventory.

## PlacementObservationSnapshot

Owned by the native User planner after Core ACK authentication and DI offer signature verification. It combines:

- request identity: `request_id`, `attempt`, `service`, `model_digest`, `graph_digest`, `candidate_set_digest`, candidate identity and plan deadline;
- one verified V4 offer digest and Provider boot epoch per admitted Provider;
- requester-local `ACK_RTT` / `RESPONSE_RTT` / `DATA_FETCH` samples, labelled by exact kind. `ACK_RTT` includes Provider ACK work and is not path-only RTT. `DATA_FETCH` endpoints follow the actual Data payload direction; a row without current implementation support or a valid byte/time denominator remains unavailable;
- Provider-reported directed Provider-to-Provider samples;
- request-scoped residency/resources/worker-capacity and the immutable role graph with nullable tensor byte estimates;
- `captured_at_ms`, `expires_at_ms`, canonical snapshot digest and source digests.

Snapshot expiry is the minimum of request deadline, offer expiry and every observation used in its cost calculation. Rebuilding a snapshot changes its digest. Rejected offers or stale rows never enter it.

## PlacementCostBreakdown

Owned by the strategy result; diagnostic only and never authority. It records selected strategy/version, snapshot digest, User control-path cost, data-flow critical-path estimate, per-role feasibility and node cost, each directed graph edge and byte estimate, User ingress/egress edges actually present, residency state, decision duration/evaluated assignment-state count, deterministic tie-break, and fallback reason. It omits raw Data names and secrets. The sealed placement binds the snapshot digest and selected assignments; the detailed cost record is not a grant or lease.
