# Traceability: Stream/Prefetch API Simplification

The executed evidence for every row is consolidated in
[evidence/post-implementation-audit.md](evidence/post-implementation-audit.md).

| Requirement | Task(s) | Closure acceptance |
|---|---|---|
| FR-001 | T002-T004 | Existing API builds/tests unchanged |
| FR-002 | T002 | C++/Python create entry-point parity |
| FR-003 | T002 | Frozen publisher method surface |
| FR-004 | T002 | Minimal config validation |
| FR-005 | T002 | Fresh session and versioned-prefix vectors |
| FR-006 | T002 | Canonical generic-name vectors |
| FR-007 | T002 | Explicit later-announcement delegation |
| FR-008 | T002 | Off-Face readiness plus announce/publish/activate bootstrap |
| FR-009 | T002 | Prior-announcement publication delegation |
| FR-010 | T002 | Atomic failure and no silent re-announcement |
| FR-011 | T003 | C++/Python subscribe entry-point parity |
| FR-012 | T003 | Existing handle open/start/return delegation |
| FR-013 | T003 | Subscription option field parity |
| FR-014 | T003 | Descriptor-aware default vectors |
| FR-015 | T003 | Existing item/admission callback semantics |
| FR-016 | T002 | Bounded, concurrency-safe facade lifecycle |
| FR-017 | T004 | No algorithm/default change |
| FR-018 | T002-T003 | Full C++/Python facade parity |
| FR-019 | T002-T003 | Paste-ready Provider/consumer examples |
| FR-020 | T004 | Full regression, CodeGraph, neutrality, frozen hashes |
| SC-001 | T002 | Provider after-example lifecycle call count |
| SC-002 | T003 | One-call auto-start subscription |
| SC-003-SC-004 | T002-T003 | C++/Python field and delegate parity |
| SC-005-SC-007 | T004 | Regression, code-aware audit, frozen evidence |
