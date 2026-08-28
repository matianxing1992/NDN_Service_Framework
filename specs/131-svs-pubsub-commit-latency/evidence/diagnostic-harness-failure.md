# Diagnostic Harness Failure: confirm01

Campaign `spec131-confirm01-20260722T023252Z` contains 50 immutable diagnostic
cells, but it is excluded from Spec 131 performance conclusions.

The original driver generated offered load by scheduling publication callbacks
on the same Face `io_context` that performed NDN-SVS work. A callback more than
one period late was skipped. Baseline skipped 42.61%, 60.04%, 71.19%, 73.50%,
and 72.15% of scheduled release slots at 200, 400, 600, 800, and 1000 pps,
respectively, even though publish API p50 remained approximately 0.07-0.09 ms.
The analyzer then treated only `api-return` events as emitted, so its delivery
ratio did not expose never-attempted scheduled slots.

Therefore confirm01 proves a harness scheduling defect. It does not establish
the baseline or latest NDN-SVS throughput limit. The corrected experiment uses
an independent Face thread, independent high-resolution pacer thread, a common
thread-safe `io_context::post` publication adapter, and separate
attempted/scheduled and delivered/attempted ratios. Formal admission requires
both subjects to reach
1000 pps attempted rate within +/-2% before exactly 10 new cells are sealed.
