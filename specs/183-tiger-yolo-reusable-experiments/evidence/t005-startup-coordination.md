# T005 startup coordination checkpoint

2026-09-07. PARTIAL; no native NFD/SIF/Tiger/model run.

The production `start_workload` coordinator now calls existing actual
Controller, publication-readback, Repo, signed-network and Provider helpers in
dependency order. Two workers share atomically published run/candidate/probe-
bound control records, a monotonic startup budget and peer failure indicators.
Both signed-network receipts must match opposite Provider identities before
Controller startup. Providers cannot start until Controller publication and
Repo capability readiness; the full per-node Provider set must be ready before
returning RUNTIME_READY. Errors are published without replacing the first
exception, and the outer owner must always tear down its children.

Application naming follows the user's correction: `/<appName>/sync`. The plan
records applicationName and the prepared config records runtime.application_name;
group is derived from it, not provider_prefix. The default Spec183 appName is
its isolated run namespace; the old template's application display label is
not an NDN routing prefix. Custom application subnames remain separate from
Provider identity names. Route-ready records using `/group` or a different
Sync prefix fail before application startup. No legacy NFD route was executed
or silently treated as correct.

Focused evidence: nine new coordination cases use real filesystem records and
two concurrent threads with explicit application/Worker doubles. They cover
stale/symlink/missing/failed/duplicate records, correct two-rank ordering,
wrong Sync prefix, wrong peer and a Provider permission-readiness failure.
An additional preparation test makes the application name different from the
Provider prefix and requires the resulting appName/sync group.

The follow-up hardening centralizes this construction in
`runtime.yolo_profile.application_sync_prefix()`. Projection, NFD route setup,
and startup validation now share the same absolute-name validator; malformed
names, trailing slashes, duplicate separators, and relative names are rejected
before any runtime launch. This remains a routing-integrity check, not a native
or Tiger execution result.

Full focused suite: 412 passed in 26.13s, JUnit at
Experiments/TigerCluster/results/t005-startup-coordination/junit.xml.
After tightening barrier rank types and directory rechecks, the 15 affected
startup/preparation cases passed again in 1.00s.
These are component tests, not native runtime validation. Final outer worker
NFD/route installation, submission wiring, complete T006 request evidence and
T007 audit still block formal runtime gates. T005 remains unchecked.
