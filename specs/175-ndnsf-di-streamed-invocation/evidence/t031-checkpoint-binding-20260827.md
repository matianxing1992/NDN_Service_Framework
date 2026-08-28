# T031 local correction: staged checkpoint binding

Date: 2026-08-27

The native `ConversationStateStore` staged-promotion paths now keep checkpoint
authority explicit. The legacy `commitStagedPromotion(binding)` overload
rejects a binding that already carries a checkpoint; the authenticated
`commitStagedPromotion(binding, checkpointDigest)` overload validates a
caller-supplied non-empty binding checkpoint and accepts it only when that
digest equals the aggregate checkpoint supplied by the commit control. A
conflicting digest is rejected before the staged entry can become `IDLE` or
erase its request-local owner.

This is a local invariant correction, not a claim that the real multi-Provider
receipt/commit flow is complete. The existing empty-checkpoint staged shape is
preserved: the control path resolves a staged binding, then supplies the
checkpoint exactly once at commit.

Verification:

```text
./waf build --target=unit-tests -j1                 -> exit 0
./build/unit-tests --run_test='Spec175*,ConversationState*' -> 33 cases, no errors
git diff --check                                    -> exit 0
```
