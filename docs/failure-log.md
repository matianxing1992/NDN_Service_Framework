# Failure Log and Evidence Index

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

The `r35` result is the newest active blocker. Its P/E logs show the
Controller issuing `registering prefix: /example/controller` and the six
NDNSF filters, but no successful Controller-prefix registration and no
`PUBPARAMS` filter before the Python readiness timeout. This is classified as
a startup/environment boundary failure, not as an ACK disposition failure.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
