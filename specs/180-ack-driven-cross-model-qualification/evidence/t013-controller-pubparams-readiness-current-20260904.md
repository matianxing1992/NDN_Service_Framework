# T013 Controller PUBPARAMS readiness repair (2026-09-04)

## Status

This is implementation/wiring evidence for the current source tree. It is not
local qualification, exact-SIF, or Tiger execution evidence.

## Closed blocker

The revision-122 node-local SIF reproduction stopped in the Controller before
the publication User could fetch NAC-ABE public parameters. The reproducible
artifacts were `/tmp/rs-out/controller.log` and
`/tmp/rs-out/orchestration-terminal.json` (`exitCode=7`). The failure occurred
because NAC-ABE installs the `PUBPARAMS` Interest filter from the asynchronous
success callback of AttributeAuthority prefix registration, while the
publication User was constructed immediately after the Controller background
thread was launched.

## Repair

- `ndn-service-framework/ServiceController.cpp:226-275` now keeps the existing
  NDNSF registrations, then probes the real controller `PUBPARAMS` Data name
  on the same Face/event loop before `start()` returns. The bounded probe uses
  `MustBeFresh`, `CanBePrefix`, finite retransmission, a ten-second deadline,
  and heap-owned callback state so timeout callbacks cannot reference dead
  stack variables.
- `pythonWrapper/src/ndnsf/_ndnsf.cpp:3926-4048` now exposes a readiness
  condition to the Python wrapper. Background startup reports native startup
  failures through `wait_until_ready()`; blocking `run()` preserves exception
  propagation. `stop()` wakes readiness waiters.
- `pythonWrapper/ndnsf/service.py:2304-2340` waits for native readiness in
  both `start()` and `start_background()` and stops the native Controller on
  timeout or startup failure.

## Verification

The following checks passed against the current source:

```text
./waf -o build-system-j2 build -j2                         PASS (274/274)
NDNSF_LIBRARY_DIR=.../build-system-j2 python3 setup.py      PASS (_ndnsf rebuilt)
PYTHONPATH=.../pythonWrapper pytest -q \
  tests/python/test_spec180_yolo_application.py \
  tests/python/test_spec180_release_workflow.py             PASS (22 passed)
PYTHONPATH=.../pythonWrapper python3 -c 'import ndnsf._ndnsf' PASS
```

The rebuilt extension has an explicit `build-system-j2` RUNPATH; `ldd` found
no missing libraries and resolved Boost 1.71 plus the current core/NAC-ABE/
NDN closure. The current core and extension digests are recorded by the
working-tree verification command, not inferred from the old SIF.

## Candidate invalidation and next gate

The behavior-affecting Core/Python runtime change invalidates the prior exact
SIF evidence (`c7c84006ade8c3d657e95ad888589ac748cead5a17738856337e842fee20ec77`)
under the immutable-candidate change-plane rules. No old SIF/Tiger result is
reused. Ownership returns to T013; the required order is a fresh T014
design-code convergence PASS, one current-source T015 local qualification,
then a new candidate/SIF and only afterward any Tiger action. No SIF or Tiger
experiment was rerun for this repair.
