# Spec189 provider dependency timeout wiring checkpoint

## Scope

The r48 run showed that the provider executable accepted
`--repo-fetch-timeout-ms`, but the value was not assigned to the native handler
configuration. The first repair incorrectly reused `fetchTimeoutMs`, which also
bounded the post-Selection readiness barrier and conversation control ACK
deadline. The final repair adds `dependencyFetchTimeoutMs` and keeps those
control-plane budgets independent.

## Review and validation

- Frozen review snapshots r4 and r5 passed the read-only review-agent gate;
  r6 (including the environment-override selector) also passed `STATIC_PASS`.
  The final snapshot is `.codex-tmp/spec189-provider-timeout-review-r6/` with
  diff SHA-256
  `774ba576d82bb4b469f8ad883b5090c374491d5bba2d0dc493ba35402d305660`.
- The first unified build attempt (`build-r1`) stopped at the C++ compile
  boundary because the public timeout-budget declaration conflicted with an
  anonymous-namespace definition. No link or runtime assertion ran. The repair
  moved the public wrapper outside that namespace; the failure remains in
  `.codex-tmp/spec189-provider-timeout-build-r1.log`.
- The affected NDNSF targets then rebuilt with the repository Waf in the
  existing `build-spec189-b189-3-global-r3` tree using `-j4`:
  `ndnsf-distributed-inference`, `di-native-provider`, and `unit-tests` passed
  in 3m04.100s (`build-r2`). After adding the environment regression selector,
  `unit-tests` rebuilt in 38.839s (`build-r3`).
- The C++ selectors
  `NativeProviderTimeoutBudgetKeepsDataAndControlDeadlinesIndependent` and
  `NativeProviderTimeoutEnvironmentOverrideStaysOnDependencyFetch` passed in
  `.codex-tmp/spec189-provider-timeout-selector-r2.log`.
- The rebuilt provider `--help` output retains
  `--repo-fetch-timeout-ms <ms>`; Python AST parsing and `git diff --check`
  passed.

## Contract boundary

`--repo-fetch-timeout-ms` now populates only the authenticated dependency
fetch budget used by `NdnsfCollaborationDependencyIo`. Readiness and
conversation-control deadlines use `fetchTimeoutMs` and are not extended by
the large-interest environment override. Defaults remain 30 seconds.

This is a local wiring and timeout-policy checkpoint. It does not prove that
the next Qwen request completes; no new MiniNDN run, runner-ready marker,
terminal response, output, or qualification PASS is claimed here. A fresh run
must use a new run ID and retain the existing r48 failure boundary if another
stage fails.
