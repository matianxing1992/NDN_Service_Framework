# Pre-Implementation Audit Checklist: Spec 114

**Date**: 2026-07-16  
**Verdict**: PASS to begin implementation

## Intent and Scope

- [x] The feature directly addresses V3 wire incompatibility.
- [x] V2 compatibility is explicit and isolated, not an automatic fallback.
- [x] Spec 113 reliability work is preserved as a separate concern.
- [x] Remote merge, push, release, Docker, iTiger, and physical-network work are excluded.

## Architecture and Correctness

- [x] One profile/options source owns version and timer defaults.
- [x] V2/V3 codecs are separate while the state machine remains shared.
- [x] The V3 signed/validated object is the embedded Data.
- [x] Invalid vectors apply no partial state and cannot escape the event loop.
- [x] Core V3 is separated from Mapping/Repair extension state.
- [x] No Sync Ack Data is emitted.

## Migration and Consumers

- [x] Source compatibility and ABI rebuild expectations are explicit.
- [x] NDNSF exposes explicit V2/V3 selection.
- [x] Missing suppression override preserves the selected profile default.
- [x] The shared GUI adapter no longer silently injects 1 ms.
- [x] English/Chinese documentation and affected consumer regressions are tasked.

## Validation and Evidence

- [x] Production encoders are not used to generate their own golden oracle.
- [x] Positive, malformed, security, version, restart, and extension vectors are covered.
- [x] Independent C++/NDNts exchange is bidirectional.
- [x] NDNts timers and test signing are pinned explicitly.
- [x] MiniNDN matrix identity, run-once rules, stop conditions, and negative-result retention are defined.
- [x] Every FR and SC maps to tasks and planned evidence.
- [x] Strict structural scan passes.

## Implementation Gate

- [x] No open Critical or High design finding.
- [x] No unresolved clarification or placeholder.
- [x] The 29 tasks are sequential, dependency-ordered, behavior-centered, and path-specific.
- [ ] Implementation evidence exists. *(Intentionally false before T001.)*
