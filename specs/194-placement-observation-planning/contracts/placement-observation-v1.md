# Placement Observation V1 Contract

**Status**: `DRAFT`<br>
**Owner**: Core owns generic sample capture and directed transport facts; DI owns model/residency interpretation, immutable planning snapshot, cost model and placement.<br>
**Compatibility**: Existing `DI_PLACEMENT_V3` canonical bytes and signature validation remain unchanged. V1 observations are carried only through a separately versioned `DI_PLACEMENT_V4` offer and native proposal.

## Request Version Selection

The User sends `placementProfile=DI_PLACEMENT_V4` only when every Provider in the frozen eligible candidate set advertises V4 support. One request/attempt has one offer schema; V3 and V4 offers are never mixed. If capability coverage is incomplete before the request, use the existing V3/default request path and record `OBSERVATION_UNAVAILABLE`. If V4 offers authenticate but their observations fail freshness/coverage checks, run the current baseline ranking semantics over the authenticated V4 non-network offer fields and record the fallback reason; do not translate or relabel their bytes as V3. Exact capability source and request-builder signature are frozen at T001.

## V4 Offer Envelope

The V4 offer retains request/provider/status/device/resource identity and signing requirements from the V3 offer, adds the explicit observation version and request-scoped resource/capacity/residency/network fields, and represents unavailable scalars or unknown collections with `null`. This sketch is normative for semantics; T001 freezes exact canonical ordering and signature bytes before implementation.

```json
{
  "schema": "DI_PLACEMENT_V4",
  "schema_version": 4,
  "request_id": "/request/name",
  "attempt": 1,
  "service": "/service/name",
  "provider": "/provider/name",
  "model_digest": "sha256:...",
  "graph_digest": "sha256:...",
  "candidate_set_digest": "sha256:...",
  "boot_epoch": "provider-boot-id",
  "captured_at_ms": 0,
  "expires_at_ms": 0,
  "resources_state": "UNKNOWN",
  "resources": null,
  "capacity_state": "UNKNOWN",
  "capacity": null,
  "queue_depth": null,
  "estimated_wait_ms": null,
  "residency_state": "UNKNOWN",
  "residency": null,
  "link_observations": [],
  "signer_key_id": "sha256:...",
  "signature": "base64-ed25519"
}
```

Each `link_observations[]` row follows `NetworkObservation`, including required `direction_semantics`. A row reported by Provider P MUST have `observer_peer == offer.provider`. For a real dependency `DATA_FETCH`, Core stores the lookup as consumer Provider / producer Provider, but V4 exports `src_peer=producer` and `dst_peer=consumer` because those are the Data payload's source and receiver; `observer_peer` remains the consumer. For `ACK_RTT`, `src_peer` and `dst_peer` identify requester and responder and `direction_semantics=REQUESTER_RESPONDER_ROUND_TRIP`. It is unusable if endpoint identity, boot/topology binding, timestamps, sample count, scope or canonical source digest fails validation. Missing edges are omitted, not emitted with fabricated numbers.

`resources` contains only request-relevant measured resource rows; `capacity` contains only the measured worker/capacity summary needed for the current candidate roles. Each has `OBSERVED`, `KNOWN_EMPTY`, or `UNKNOWN` state; `UNKNOWN` uses `null`, `KNOWN_EMPTY` uses an empty array, and `OBSERVED` uses bounded rows. Rows bind to provider boot/process and offer expiry. T001 freezes exact row fields from the real `NativeProviderTelemetryCollector` and readiness types; no arbitrary host inventory is allowed. Queue and wait are separately nullable observations.

## User-Local Observations

The User planner adds requester-local observations to the local immutable snapshot after ACK arrival. `ACK_RTT` measures request-to-ACK elapsed time, including Provider ACK work. It can rank service-path responsiveness only; it cannot price a tensor-sized transfer. `DATA_FETCH` uses measured bytes and elapsed time from a real transfer, with endpoints following payload source → receiver. A `DATA_FETCH` row with no current implementation path or denominator remains unavailable.

## Snapshot and Strategy Binding

The native planning call receives `const PlacementObservationSnapshot&` alongside the graph/candidate and verified admitted offers. The proposal records `observation_snapshot_digest` in addition to request/attempt, ACK-closure and strategy identity. A sealer/selection record rejects a proposal if any bound digest or deadline differs. Only verified current offers populate `providers`; decoded JSON alone has no authority.

For candidate assignment A, hard admission and resource feasibility run first. On a known dependency DAG, the estimator uses only edges whose tensor `estimatedBytes` and directed transport observation are available. For an edge with producer role `u` on provider `p` and consumer role `v` on provider `q`:

- `p == q`: no network transfer term;
- `p != q`: `DATA_FETCH` observations price `first_byte_ms + (estimatedBytes * 8 / (goodput_mbps * 1,000,000)) * 1,000`; retry/Nack facts are reported separately and may only affect cost through a frozen, measured formula;
- missing edge size/observation: assignment is not scored by synthetic defaults.

User input/output data terms are added only when the immutable plan declares an actual transfer to/from that endpoint and supplies a known byte estimate. Node preparation/reuse uses request-scoped residency and explicit assembly/load estimates; queue/capacity uses observed values only. The assignment score is `controlPathMs + dataFlowCriticalPathMs`: `controlPathMs` is the maximum requester-local `ACK_RTT` among distinct assigned Providers and remains labelled as request-to-ACK service path; `dataFlowCriticalPathMs` is the longest path over role node and dependency edge estimates. It excludes model inference time and is not measured end-to-end latency.

The V4 strategy explores assignments with deterministic beam order over graph topological roles, preserving the current V3 preferred-provider/distinct-provider choice groups. `beam_width` and `max_assignment_states` are explicit V4 strategy inputs. It returns the minimum score among retained complete assignments, with provider/device/backend lexical tie-breaks; it is exact when the complete assignment set fits inside both bounds, and otherwise is a bounded heuristic. `NativeCandidateBudget.maxPolicyMs` and the request deadline are additional hard limits. A budget/deadline stop falls back to the existing strategy and records the reason; it never returns a partial assignment. No score is labelled as globally optimal after beam pruning or as measured end-to-end latency.

If no complete candidate is scoreable before `NativeCandidateBudget.maxPolicyMs` or the request deadline, retain the negotiated V4 profile and invoke the existing baseline ranking semantics over the authenticated V4 non-network offer fields; report `OBSERVATION_INCOMPLETE`, `OBSERVATION_STALE`, or `BUDGET_EXHAUSTED`. The V3/default path is selected only before the request when full-roster V4 capability is unavailable. Never downgrade/mix offer schemas, or relax identity, admission, authorization, lease, device, memory or cancellation checks to keep the optimization active.

## Authentication, Freshness, Privacy, Failure

- Core ACK authentication MUST validate the transport signer/provenance; DI MUST independently validate the V4 canonical offer signature against the configured provider/service/key policy.
- Bind request ID, attempt, service, model/graph/candidate-set digest, provider identity, provider boot epoch and offer deadline. Where the destination epoch/topology was not observed, the corresponding link is unusable for V1 optimized scoring.
- Use the telemetry source TTL; cap each row by request/offer expiry. A current signature does not make an old sample fresh.
- Reject malformed numbers, negative/non-finite values, duplicate directed keys, direction swaps, digest mismatches and unknown mandatory schema fields.
- Limit V4 report rows to current candidate Providers/edges. Do not publish raw Data names, internal authorization scope strings, model/input bytes, KV, grant/key material, or unrelated cache inventory.
- No observation collection fetches or assembles full model material before Selection. Failed or unavailable telemetry falls back without changing protocol authorization state.

## Planned C++ Oracle Cases

The C++ fixture must call the named production capability selector, V4 decoder/admission, snapshot builder, strategy and proposal validator. Its independent expected result covers: incomplete/expired/mixed-version capability roster, reversed edge, expired row, current-but-wrong boot, changed topology, signature/source mismatch, missing byte size, explicit same-provider edge, unequal directed edges, known-empty versus unknown resource/capacity/residency, deterministic tie, budget exhaustion and baseline fallback while retaining the negotiated profile. A Python harness may launch the target but cannot assert native protocol/placement behavior.
