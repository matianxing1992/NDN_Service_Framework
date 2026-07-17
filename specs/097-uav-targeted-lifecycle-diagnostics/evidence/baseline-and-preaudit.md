# Baseline And Pre-Implementation Audit

## Reproduction

Spec 096 preserves two exact post-exit aborts:

```text
control-only-run-01: GS_GUI_EXIT rc=0 -> terminate called without an active exception
combined-fec1-run-03: GS_GUI_EXIT rc=0 -> terminate called without an active exception
```

The control-only MiniNDN mode is the fastest correct feedback seam and retains
the real ServiceController, provider, user, Targeted request, GUI automation,
and shutdown path.

## Code Reality

- `shutdownRuntime()` sets `m_done` but checks/joins YOLO, decoder, playback,
  and refresh workers before stopping/joining the face thread.
- The face thread creates `m_yoloPrewarmThread` after runtime initialization.
- A late creation can therefore occur after the worker join check; later
  shutdown calls return early because `m_done` is already true.
- The existing command path records final UI status but does not correlate
  queue/dispatch/response/timeout stages with request ID and elapsed time.

## Audit Verdict

**PASS**. The face-first quiescence change is minimal and preserves protocol,
security, and application behavior. Diagnostics are metadata-only and exclude
payload/token/certificate/key material. The 0%/5% control-only matrix separates
lifecycle correctness from remaining network delivery failures.

## Implementation And Diagnosis

Implemented changes:

- face-first runtime quiescence with permanent `GS_RUNTIME_SHUTDOWN` phases;
- payload-free `GS_TARGETED_PHASE` and `UAV_CONTROL_COMMAND` diagnostics;
- parser support for both `terminate called without an active exception` and
  `__pthread_tpp_change_priority`, including `lifecycleAbortReason` and an
  explicit rule that any lifecycle abort rejects an otherwise successful run;
- owned/joined auto-MAVLink worker whose actions enter through the GTK context;
- serialized Ground Station `ServiceUser` callbacks on the Face thread. The
  object-detection provider remains parallel because it performs heavy work.

Observed diagnosis sequence:

1. `results/spec097-uav-targeted-control-loss00-final`: 5/5 accepted, 5/5
   command completion, zero aborts after face-first quiescence.
2. `results/spec097-uav-targeted-control-loss05-final`: 1/5 accepted, 1/5
   command completion, three glibc pthread assertions. Reparse now reports all
   three as lifecycle aborts instead of ordinary command failures.
3. `results/spec097-uav-targeted-control-loss05-mainthread-smoke`: moving only
   automation actions to GTK still produced one pthread assertion. This
   falsified the detached/direct-command hypothesis as the complete cause.
4. GDB-attached runs changed timing and did not reproduce SIGABRT. One completed
   normally; one shut down cleanly but failed the command-completion criterion.
5. ASan/UBSan compilation was attempted. Parallel debug compilation exhausted
   the 11 GiB host and was terminated by the OOM killer. The build was restored
   with normal configuration and `-j2`; no sanitizer result is claimed.

## Current Verification

Passed on the current source:

```text
./waf build -j2 --targets=UavGroundStationApp
./waf build -j2 --targets=App_ServiceController,UavDroneApp
./waf configure --with-examples --with-tests
./waf build -j2 --targets=unit-tests,di-native-provider,di-native-plan-schema-smoke,di-native-plan-manifest-smoke,di-native-provider-session-smoke
./build/unit-tests --log_level=test_suite
python3 tests/python/test_ndnsf_uav_stream_control_isolation_campaign.py
```

Results: all required runtime binaries rebuilt; all 216 C++ tests passed; 14
focused Python tests passed; the diagnostic sensitive-field scan passed. The
C++ suite now explicitly verifies that `ServiceUser::setHandlerThreads(0)`
runs the response callback inline on the caller/Face thread after pending-call
state has been removed.

Campaign aggregation now reports command outcomes as
`command -> latest stage -> count` and separately counts attempts that never
reached `response`, `timeout`, `blocked`, or `busy`. A control run with such an
unterminated attempt is rejected even when the launcher exits cleanly. Reparse
of the copied historical 5% evidence correctly identified one
`emergency_stop` left at `attempt`; the canonical historical result directory
was not modified.

Full Python discovery executed 352 tests. It produced 342 passes, one skip, and
10 environment errors: nine Tk tests could not connect to the Xvfb display and
one fake HTTP server test could not create a loopback socket because the active
sandbox denies socket creation. There were no assertion failures after the
standard example/test build targets were restored. The earlier unrestricted
run passed the full Python suite; the current focused tests cover every Python
file changed by this feature.

An earlier restricted session could only dry-run the frozen matrix because
`sudo` lacked its required owner/setuid state and network namespace operations
were denied. No host-NFD or rootless substitute was accepted. That historical
environment limitation was cleared before the final MiniNDN evidence below;
it is not the status of the completed feature.

CodeGraph exploration successfully returned the current `ServiceUser` worker
dispatch and Ground Station call paths. The standard full sync initially failed
with `Maximum call stack size exceeded`. Running the same bundled CLI with Node
`--stack_size=32768` then synced 8512 changed files and resolved references in
2 minutes 46 seconds. A subsequent standard `codegraph status .` reported
`Index is up to date`; the CodeGraph portion of T018 is therefore verified.

## Final Current-Revision MiniNDN Evidence

The unrestricted session completed two admissible, serial, no-retry matrices on
the current revision. The 0% matrix used the frozen canonical path:

```bash
python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec097-uav-targeted-control-loss00-current-final \
  --workload-modes control-only --runs 5 --loss-percent 0 \
  --auto-stop-seconds 60
```

The 5% matrix was launched through a unique temporary output path because stale
concurrent automation sessions repeatedly renamed and cleaned the frozen path
while MiniNDN was active. After the exclusive run completed, its intact result
directory was promoted to the canonical final path. The summary therefore
retains the original unique `runDirectory` strings while the artifact is now at
`results/spec097-uav-targeted-control-loss05-current-final`. All workload
parameters remained frozen:

```bash
python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec097-uav-targeted-control-loss05-current-final-exclusive-20260711_2315 \
  --workload-modes control-only --runs 5 --loss-percent 5 \
  --auto-stop-seconds 60
```

The completed summaries report:

| Loss | Runs | Accepted | Command completion | Lifecycle aborts | Unterminated attempts |
|---:|---:|---:|---:|---:|---:|
| 0% | 5 | 5 | 5/5 | 0 | 0 |
| 5% | 5 | 1 | 1/5 | 0 | 0 |

At 0%, all 20 Arm/Takeoff/Land/Emergency-stop attempts ended at `response`.
At 5%, every attempt still had an observable terminal or blocked stage:
Arm ended at four `response` and one `blocked`; Takeoff ended at one `response`
and four `blocked`; Land ended at four `response` and one `blocked`; Emergency
stop ended at five `response`. Two runs also recorded a generic Targeted
`timeout`, but no command remained at `attempt`.

Both summaries set `automaticRetry=false`, `runsPerCell=5`, and
`lossPercent` to the intended cell. A direct scan found neither
`terminate called without an active exception` nor
`__pthread_tpp_change_priority` in the ten admissible runs. The lifecycle goal
therefore passes. The 20% command completion at 5% loss is retained as a real
negative network-reliability result; this feature does not add retries, change
timeouts, or claim that Targeted control is reliable under loss.

Several directories named `*invalid*`, `*stale*`, or `*tool-interrupted*`
preserve attempts contaminated by concurrent cross-session launchers or cleanup.
They are infrastructure diagnostics, are not counted in the ten-run matrix, and
were never substituted for measured command failures.
