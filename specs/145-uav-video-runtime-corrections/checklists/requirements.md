# Specification Quality Checklist: UAV Video Runtime Corrections

- [x] The three requested defects are explicit and independently testable.
- [x] Non-30-fps behavior covers 20, 30, and 60 fps and both backend inputs.
- [x] Committed future Mapping cannot be silently relabeled.
- [x] Both GStreamer C callbacks have a complete exception contract.
- [x] Failure state, later-callback suppression, and teardown behavior are
  specified.
- [x] Actual Core state is distinguished from APP adaptation and caps.
- [x] Unavailable and stale-session status behavior is explicit.
- [x] Core/bindings are outside the planned implementation boundary.
- [x] Specs 125/126 and canonical results have recorded immutable hashes.
- [x] Historical runners and selective reruns are forbidden.
- [x] Fresh validation is exactly one new 60-second, 20-fps MiniNDN cell.
- [x] Success/failure thresholds and preservation policy are preregistered.
- [x] Spec 144 promotion is conditional on complete Spec 145 PASS.
- [x] Out-of-scope lead-time/Mapping/retry/FEC work is explicit.
- [x] Every functional requirement maps to a task and verification evidence.

**Result**: PASS for implementation planning. This checklist does not claim
that implementation or MiniNDN validation is complete.

