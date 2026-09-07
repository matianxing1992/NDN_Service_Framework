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
