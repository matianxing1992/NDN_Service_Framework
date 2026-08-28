# Traceability: NDNSF Stream Live Prefetch

| Source intent | Requirements | Design owner | Acceptance |
|---|---|---|---|
| Keep immutable meaningful application names as Data names | FR-001, FR-013, FR-021, FR-024, FR-026 | application naming policy + Core resolver/network facade | versioned-name/stale-cache round-trip and no synthetic fallback |
| Preserve predictable future exact-name prefetch | FR-002, FR-003, FR-014, FR-020 | fixed-block Core map + Provider publication/retention | cursor/block formula, retained-chain eviction and Provider future-hit evidence |
| Avoid packet-in-packet encapsulation | FR-002, FR-021 | StreamNameMap wire contract | map contains Name/control metadata only |
| Make receiver phases explicit | FR-004, FR-005 | existing Core adaptive state | phase-transition vectors |
| Require stable adjacent windows for the real sample unit | FR-006, FR-007, FR-008 | Core live-edge estimator | paper-literal/NDNSF profiles and publication-group vectors |
| Avoid unsafe clock arithmetic | FR-009, FR-010 | Core delay estimator | unsynchronized-clock negative vectors |
| Bound multiplicative changes and future waits | FR-011, FR-012, FR-023 | Core controller | aggregate budget, congestion, hysteresis and lifetime vectors |
| Cover variable sample item counts | FR-014, FR-015 | Core estimator + application mapping publication | later-cursor/immutable-overprediction vectors |
| Bound pending future Interests | FR-016, FR-020 | LiveStreamPublisher + Core reverse resolver | unmapped/far/cap/expiry tests |
| Recover while still useful | FR-017, FR-030, FR-031 | Core optional opaque-byte FEC + bounded retransmit | every one-loss position plus corrupt/two-loss/deadline vectors |
| Expose one reusable network API while keeping media policy in APP | FR-013, FR-018, FR-027, FR-028 | LiveStream facade + UAV callbacks | preparing/activation, async sample admission, app-neutral C++/Python/MiniNDN gate |
| Keep crypto outside Core | FR-029 | APP-before/after boundary | random opaque bytes, API/log secret scan, reject-without-estimator tests |
| Make FEC usable but optional | FR-030, FR-031, SC-009 | LiveStream group reservation/publication/recovery | FEC-off and signed digest-bound XOR gates |
| Preserve mapping and payload security order | FR-019, FR-024, SC-004 | Spec 118 admission before observation | trust identity/fork/stale-cache negative matrix |
| Treat cache/PIT and map privacy honestly | FR-023, FR-025 | Core exact-Interest policy + documented boundary | exact Interest, no-aggregation, metadata-leak tests/docs |
| Preserve rollback without two wire identities | FR-021, FR-022 | same map wire contract + Core policy | pressure-only compatibility vectors |
| Prove rather than assume improvement | SC-002, SC-005 through SC-008 | three-policy matched MiniNDN campaign over one Core API | future-hit manipulation checks, direct Mapping byte/Interest overhead, run-level effects, negative-result disposition |
