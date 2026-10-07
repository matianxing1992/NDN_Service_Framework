# Spec193 L03/L04 checkpoint

Updated: 2026-10-07 11:42 -05:00. Owner branch: Experimental.
Authority: `.specify/feature.json` and active Spec193 tasks.md.

L03/L04 local registry cases passed twice on the same candidate lock
`ee614110654bed7edd391b2d7a3ff908efc9ccb00dd1e1c77f99a3971fd54891`:
`specs/193-distributedrepo-multinode-api-campaign/evidence/l03-l04-diag-003/004`.
Both five-host runs have zero-exit native child, five nonempty pcaps and cleanup
PASS. Independent signature, source/physical payload, missing/corrupt/restored
segmented reads and zero remote-call checks passed. Product C++ was unchanged.

Design and unique result record:
`specs/193-distributedrepo-multinode-api-campaign/contracts/local-registry-campaign.md`
and `evidence/l03-l04-review-20261007/review.md` under that feature. Failed 001/002
remain FAIL and are recorded in failure-log.md; fixes corrected fixture/oracle
representation assumptions. Unaffected product unit/integration evidence was reused.

Full Spec193 is still incomplete: T001/T008 DONE, T002–T007 open. Next N01–N10,
then P01–P06 and A19/A21/large-range gaps, fresh complete audit/regression, and
one immutable candidate running the full untraced matrix twice. Current native
fixture/runner/oracle remain in the existing untracked Spec192/193 composition;
full source closure must be reviewed and committed before clean-checkout claims.
Preserve unrelated dirty source and evidence; no push.

Context Mode strict health, CodeGraph/source, Spec Kit structure/shared-entrypoint
checks and official inline read-only review were used. GSD historical state was
already restored for continuity; active Spec registry remains checkpoint authority.
ARS is not applicable to these deterministic functional conformance tests.
