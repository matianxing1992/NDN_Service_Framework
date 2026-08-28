# US3 Targeted Total Deadline

## Disposition

Email defect 4 was reproducible in the current C++ implementation: the request
timer was created only after `PublishRequestV2` returned. A Targeted call could
therefore wait indefinitely in adaptive admission or during a non-returning or
throwing publication without consuming `timeout_ms`.

The correction is intentionally Targeted-only:

- the pending call records an absolute deadline at creation;
- its timer is scheduled before admission, token bootstrap, or publication;
- Targeted calls do not receive the ordinary late-pipeline grace extension;
- response and timeout remove the same pending-call owner, cancel the timer,
  and clean queued admission state once;
- the Python synchronous fallback is bounded by `timeout_ms + 500 ms`;
- the Python asynchronous boundary uses one atomic response-or-timeout claim.

Normal request timeout semantics were not changed.

Evidence level: **executed-pass (C++ unit + rebuilt compiled Python binding +
0% MiniNDN Provider-degradation fault injection)**.

## Test-First Result

Before the source fix,
`TargetedDeadlineStartsBeforePublicationReturns` failed with
`timeoutCallbacks == 0` after an injected publication exception and 40 ms of
event processing. After the fix, the complete Targeted suite passed:

```text
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation --log_level=test_suite
Running 18 test cases...
*** No errors detected
```

The new cases cover publication exception, admission-queue delay, response
before timeout, timeout before late response, duplicate response, exactly one
terminal callback, pending-call removal, timer cancellation, and queued-state
cleanup.

Rebuilt artifacts:

- `build/unit-tests`:
  `b90e3c872dcb478cb2a673868ab1d0e6faea0b9a7cd70bc6f51f1c6b92544fec`
- `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so`:
  `0fa7c6e7b8a235a15666fd0bfe7fbfa7d488f47024162b7ca7347935652a25f9`

The non-network Python tests loaded that real `.so`, verified both Targeted
entry points, and passed. The opt-in network test below exercised both entry
points rather than a Python fake.

## MiniNDN Provider-Degradation Evidence

Candidate: `spec112-7d6471b217bc3bdd4efe`

- manifest:
  `results/spec112-segmented/spec112-7d6471b217bc3bdd4efe/candidate-manifest.json`
  (`a81505e04d9270ce2067299610d882ed24c539aff5bf24ea386fa6d6c14bf76e`)
- aggregate:
  `results/spec112-segmented/spec112-7d6471b217bc3bdd4efe/campaign-summary.json`
  (`4724e7d4652583d4ca5fbc17ea5acba94b543b7453b5214282243973564d1cbc`)
- cells CSV:
  `results/spec112-segmented/spec112-7d6471b217bc3bdd4efe/campaign-cells.csv`
  (`62cc5ad83f4465c35a814a1eb4427b7a591105120295ad1aac2f3f8eda386ec3`)

Command:

```text
SPEC112_RUN_TARGETED_TIMEOUT=1 \
SPEC112_CANDIDATE_MANIFEST=$PWD/results/spec112-segmented/spec112-7d6471b217bc3bdd4efe/candidate-manifest.json \
PYTHONPATH=pythonWrapper \
python3 tests/python/test_spec112_targeted_timeout.py -v
```

The test driver starts each MiniNDN cell with `sudo -n -E` and a
candidate-specific ownership lock. Each cell first completed one 64-B Targeted
request, then killed that same Provider epoch and issued one more request.

| Cell | First response | Degraded request | Limit | Timeout callbacks | Response callbacks | Terminal callbacks |
|---|---:|---:|---:|---:|---:|---:|
| `targeted-timeout-sync` | 155.525 ms | 1008.010 ms | 1500 ms | 1 | 0 | 1 |
| `targeted-timeout-async` | 222.966 ms | 1067.725 ms | 1500 ms | 1 | 0 | 1 |

Both cells are `SUCCESS`; neither restarted the Provider. The 500-ms margin is
an acceptance allowance for event-loop and harness scheduling, not an extension
of the native deadline.

## Preserved Harness Failures

Two earlier candidate directories remain immutable and are not counted as
runtime evidence:

- `spec112-0e2e891612a244a5f8d8`: the default lock was root-owned, so the cell
  stopped before MiniNDN ownership acquisition;
- `spec112-89961fd616d70cf2ac10`: the driver omitted sudo, so MiniNDN rejected
  startup before topology execution.

The harness was corrected, new candidate identities were generated, and no
failed cell was overwritten or rerun.
