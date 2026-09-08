# T010 cancellation and cleanup ownership

Date: 2026-09-08. Baseline: 3202e672 plus this checkpoint.
Scope: maintained generic MiniNDN driver, component checks only. N2 remains open.

The CLI now installs scoped SIGINT/SIGTERM handlers around live execution.
Cancellation raises the driver's RunnerError, unwinding its existing finally
cleanup. Further cancellation signals are ignored during that unwind; the
original handlers are restored on exit. Cancellation returns UNQUALIFIED, 2.
An eventual external supervisor must still enforce the hard cleanup deadline.

Removed host-global `Minindn.cleanUp()` calls from startup, partial-start failure
and teardown. The installed `/home/tianxing/NDN/mini-ndn/minindn/minindn.py`
implementation invokes `nfd-stop` and `mn --clean` from that static method.
Its instance `stop()` instead visits its registered application callbacks and
stops its own Mininet instance; those owned paths remain in use. No installed
MiniNDN source was changed. Actual simultaneous-network isolation is untested.

Evidence: `Experiments/TigerCluster/results/spec183-minindn-cancellation-20260908`.
`focused.xml` records six component cases: four passed, two failed only because
the new test expected Python 3.8's Signals enum string instead of the numeric
signal received by the actual handler. Both had already completed their cleanup
assertions. The expectation was corrected to `int(sig)`; `signals-final.xml`
records both passing. No rerun of the four unaffected cases.

The signal cases deliver real SIGINT/SIGTERM to the test process through the
production CLI handler, with a workload double that checks cleanup completion
and repeated cancellation. Network teardown checks use owned-handle doubles.
These are not real MiniNDN or GPU qualification.

Still required before N2 closure: one outer execution/cleanup budget, verified
exit and descendant handling (legacy stop_process_group kills without a final
wait), retained cleanup failure semantics, three registered cases and N1's
same-run host semantic receipt. The current wrapper remains unbounded and Y-B
only; this checkpoint alone cannot release T007 or qualify T010.
