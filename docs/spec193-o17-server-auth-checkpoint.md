# Spec193 O17 server admission checkpoint

Updated: 2026-10-07 11:09 -05:00. Branch owner: Experimental.
Authority: `.specify/feature.json` → `specs/193-distributedrepo-multinode-api-campaign/tasks.md`.
Goal remains completion of all Spec193 tasks; T001/T008 DONE, T002–T007 open.

Native driver now probes both DATA_RECEIPT and DATA_LOCATION through real signed,
encrypted ServiceUser publication, bypassing client admission without UserToken.
It checks exact request-name make/parse round-trip before publishing. The runner
requires explicit logging opt-in, validated publication and decoded rejection at
both Repos, zero handler invocation, unchanged storage, independent original Data
route evidence, zero-exit children and cleanup. This is an opt-in diagnostic.

Evidence: `specs/193-distributedrepo-multinode-api-campaign/evidence/o17-server-auth-diag-003`
and `o17-server-auth-diag-004`: both five-host PASS, 12 zero-exit scenario children,
five nonempty captures and cleanup PASS. Both use candidate lock
`9fb74787d37317fd1a507f7270c809a43b87433e6da4d8295513be32cb32b921`.
At each Repo both queries were validated and decrypted, then rejected for Missing
UserToken. Offline analyzer verifies root/caller certificates and both inner/outer
RSA-SHA256 request signatures in neu/ucla/wustl captures, matching server wire hashes.
Review and analyzer: `evidence/o17-server-auth-review-20261007/` in the active Spec.

Native client SHA-256: `185fbfd44b439a9d3f3c1c6ac8824c225fbaf15e96ef8b0e51bc5d647f5bc12e`.
Runner SHA-256: `9a3318b0741143a9e4175df611d3f5515dcfb0f37b6de961edc1cb1f22527991`.
Focused native rebuild PASS (`-j4`, 27.263s); Python syntax and diff whitespace PASS.
Failed 001 (storage admission) and 002 (multi-component ID) remain FAIL with cleanup
PASS, and are recorded in failure-log.md. Only disposable Go build cache was cleared.

This checkpoint records bounded runtime evidence; the native client/runner remain
in the preexisting untracked Spec192/193 source composition. Do not claim a clean
checkout can reproduce it until that complete source dependency closure is reviewed
and committed. Preserve all other dirty work. No push has been authorized.

Context Mode active source was stale and refreshed; strict active health then passed.
CodeGraph/source and official inline read-only review were used. Spec Kit structure
and entrypoint checks passed; installed personal shared-skill copy is absent, while
repository canonical rules were loaded. Current constitution uses Spec Kit's durable
registry instead of GSD; historical GSD state was checked but does not override it.
ARS is not applicable to this functional API/security campaign checkpoint.

Remaining: L03/L04; N01–N10; P01–P06; A19 availability semantics, A21 wire-attempt
binding and larger Persistent range transfer; affected regression and fresh full
code-aware audit; finally freeze one candidate and run the full untraced matrix twice.
O17/T004/T007 remain PARTIAL/OPEN; these results qualify no TigerCluster workload.
