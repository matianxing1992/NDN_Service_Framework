# Normal node ownership wiring

Internal `apps.yolo.run_normal_node` now coordinates existing prepared Worker
helpers: configure_network → start_workload → rank-0 validated run_requests →
both-rank completion rendezvous → close → write_worker_receipt.
Rank 1 participates in the completion barrier without launching a User; it
keeps its Providers alive until rank 0 completes requests. Completion uses a
separate exclusive directory and fresh post-startup deadline while retaining
the exact startup run/candidate/probe/rank binding.

The request helper accepts an optional remaining-budget callback. Each User
child is bounded by the smaller of its frozen process timeout and remaining
workload time minus cleanup reserve. Existing callers retain their explicit
per-process timeout; there is no larger timeout or automatic retry.

Any startup/request/validation/cleanup/receipt failure attempts peer failure
notification and preserves a local exclusive node-failure.json with error type
and available cleanup rows (not secrets or arbitrary exception text). Child
teardown runs even when request validation raises. A recording failure chains
to the original exception rather than producing success.

Three orchestration tests use explicit runtime doubles to check order, request
failure cleanup/notification/recording, and rejection of a reused startup
directory. Combined with application tests: 45 passed. Full two-rank actual
runtime synchronization and negative-dependency still need acceptance.

This internal owner does not enable submit.py execution. The final operator
must supply qualified immutable inputs, exact allocation/barrier setup and a
complete request validator; callback success/node receipt alone is not
inference qualification. T005/T006/T007 remain open and no job was submitted.

Expanded regression: 572 passed in 42.79s with the established six selectors;
JUnit `Experiments/TigerCluster/results/t005-normal-node-r1/junit.xml`.

## Concurrent real-file barrier follow-up

Two threads now exercise the actual StartupBarrier filesystem protocol through
run_normal_node. The test holds rank-0 requests pending, observes rank 1 enter
its completion wait, and requires neither rank to close early. Normal release
allows both to close; injected request failure propagates through actual
failure files and produces both local failure records. Runtime/application
and receipt writers remain explicit doubles; these tests do not run NFD/SIF.

Cross-phase failure handling was tightened: a peer can fail before creating
its completion barrier, so completion checks also inspect startup failure
records without reusing the startup deadline. Finite User peer-failure checks
use that common startup failure lane, which notify() writes in every phase.
A separate regression rejects completion when only that earlier failure
record exists. Six orchestration tests pass.

Expanded regression: 575 passed in 44.67s (same six selectors), JUnit
`Experiments/TigerCluster/results/t005-two-rank-coordination-r1/junit.xml`.
