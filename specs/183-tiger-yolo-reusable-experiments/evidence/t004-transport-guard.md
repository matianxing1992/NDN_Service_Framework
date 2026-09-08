# T004.o — submission/transport exclusion

2026-09-07. Managed-journal exclusion is wired; T004 and runtime gates stay open.

Every SubmissionJournal operation now takes a shared `.transport.lock` before
its existing exclusive per-candidate lock. Receiver requires the same lock root,
takes its namespace lock exclusively and validates all correctly named journal
documents before inspecting/publishing files. Any PREPARED, SUBMITTING,
SUBMISSION_UNKNOWN, SUBMITTED or RUNNING entry rejects publication. Cancelled or
terminal history is retained unchanged. Malformed journal records fail closed.
The exclusive lock stays held through final file verification, so a concurrent
reserve cannot slip between an idle check and publication. Lock acquisition is
nonblocking with a two-second deadline per lock; no sleeping scheduler poll.

This deliberately serializes transport against the entire configured Spec183
submission namespace. It is appropriate to the sequential qualification campaign
and protects shared candidate files without trusting a caller-supplied gate key.
No journal state is cleared to permit transport. The guard does not discover
unmanaged/manual jobs; every supported submitter must use the same root/protocol.
Older frozen scripts do not implement the new namespace lock and must not run
concurrently against that namespace. Roll out the updated frozen harness only
with the namespace idle; do not treat old harness qualification as current.

Tests in results/spec183-transport-guard-20260907:
- focused.xml: 83 passed in 5.54 s, transport/journal/shared submit/terminal.
- final-lock.xml: 41 passed in 4.10 s after opening lock FDs read/write; transport
  and journal, including real independent-process contention and all active states.
These groups overlap and are not a unique-test total. No test failure this step.

Maintained `tests/transport_lock_probe.py` creates a NEW isolated root, uses only
synthetic PREPARED/cancelled journal rows, and always reaps its finite lock child.
It verifies active submission rejection, concurrent reserve timeout, successful
reserve after lock release and two-file publication after cancellation. No Slurm
query/submission, private credential, native executable, SIF or model is involved.

Actual remote probe root:
`/project/tma1/ndnsf-di/candidates/spec183-transport-guard-20260907a/probe`.
Six source files were sent in a 16011-byte archive with verified local/remote SHA
`0e77b39fb7544cf25dc5aac1a3e1379a50010b1bedd893e28fb46e01b272aa60`.
Installed operator Python ran the maintained probe with a 30-second SSH bound.
Exit 0, stderr empty; blockedActiveSubmission, blockedConcurrentReservation and
childReaped all true, transport CONTENT_VERIFIED/NOT_EVALUATED, 2 files. Receipt
scope is TRANSPORT_LOCK_PROBE_ONLY. Remote probe.json and local remote-probe.json
retain the result. Source archive and raw evidence remain in the same directories.

This proves cooperating processes on the Tiger LOGIN node using that project
filesystem path. Compute-node/cross-node locking remains an actual-allocation
check, not established by this probe. Next: semantic candidate/prerequisite
enumeration and bounded SSH/public-submit integration. Base-SIF read integrity
and the negative runner remain open; no experiment completion is claimed.
