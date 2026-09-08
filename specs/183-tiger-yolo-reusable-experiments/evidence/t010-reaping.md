# T010 tracked-child reaping and retryable cleanup

Date: 2026-09-08. Baseline 8a9b5a97 plus this checkpoint.
N2 partial source repair; no MiniNDN/network/model/SIF/GPU qualification.

The existing shared `NDNSF_DI_Yolo2x2_Minindn.stop_process_group` now signals
all supplied children, applies one shared grace/deadline budget, kills survivors
and waits again to reap them. Returned records contain actual PID, log basename,
termination request, forced status, exit status, reaped status and errors.
Unreaped children fail cleanup and retain open handles. The helper name is
historical: it owns supplied Popen handles, not arbitrary POSIX groups or every
descendant. No process-group assumption or host-global kill was added.

The maintained ACK-driven runtime no longer discards child/network handles or
sets cleanup complete before teardown succeeds. Failed partial-start cleanup
also retains its child handles for the outer finally. Retrying cleanup processes
remaining owned resources; successful resources are not stopped again. Each
attempt writes an exclusive `cleanup-attempt-NNN.json`, preserving earlier
failure observations. These records explicitly say NOT_EVALUATED and do not
replace N1's eventual semantic host receipt.

Artifacts: `Experiments/TigerCluster/results/spec183-minindn-reaping-20260908`.

- `focused.xml` / `focused.log`: 19 passed, covering relevant startup, terminal
  cleanup, runtime ownership/retry and helper cases.
- `helper-final.xml` / `helper-final.log`: four helper cases rerun after using
  one exit-status observation for both exitStatus and reaped (overlap above).
- `start-final.xml` / `start-final.log`: two partial-network-start cases pass
  after retaining a network whose first stop fails; the second cleanup succeeds.
  One case overlaps the original 19; the failing-stop variant is additional.
- Actual subprocess checks cover cooperative SIGINT exit, SIGINT-ignoring child
  killed/reaped within a one-second configured budget, and a simultaneously live
  unowned child left untouched. No native or MiniNDN processes are used.
- Unreapable-child and child/network cleanup-failure cases use explicit doubles
  to prove failure reporting, retained handles, retry and immutable attempts.

Remaining N2 work: outer execution/cleanup deadline, complete descendant and
network cleanup evidence, exactly three registered MiniNDN cases, and N1's
same-run semantic host producer/validator. Instance network.stop remains outside
this helper's child budget. No claim of bounded whole-run cleanup is made.
