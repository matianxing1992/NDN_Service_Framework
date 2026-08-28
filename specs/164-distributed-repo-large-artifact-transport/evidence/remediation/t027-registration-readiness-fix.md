# T027 Producer Registration Readiness Fix

## Outcome

PASS. The old high-concurrency `NO_ROUTE` failure is reproduced, explained,
and removed without adding an ACK reservation, resource lock, fixed sleep, or
larger protocol retry budget.

## Minimal correction

`NativeFileSegmentedObjectProducer.start()` now:

1. registers the exact producer prefix with an explicit NFD command identity;
2. processes face events and waits at most five seconds for the corresponding
   success or failure callback;
3. returns only after confirmed success;
4. stops and throws the exact registration error on rejection or timeout.

The MiniNDN artifact harness now separates persistence readiness from serving
readiness:

```text
store-ready
→ coordinator grants one serve-cold registration turn
→ producer-ready after confirmed NFD registration
→ all cold consumers run concurrently
```

Only same-NFD management-plane registrations are serialized. Publication,
storage, cold retrieval, verification, and independent Provider task queues
remain concurrent.

## Failure-focused evidence

Pre-fix real-seam reproduction:

```text
results/spec164-t026-preregistration-r1c4-before-20260730T0845Z
```

Observed: 3/4 cold results and `Nack: 150 (NO_ROUTE)`.

Callback-wait diagnostic exposed the hidden management failure:

```text
failed to register file Data prefix ...: authorization rejected
```

Single-producer control with the same management identity passed:

```text
results/spec164-t027-prefix-ready-r1c1-control-20260730T0910Z
```

Post-fix regression:

```text
results/spec164-file-producer-registration-20260730T091626Z
```

Observed: 4/4 publication/store workers and 4/4 cold-retrieval workers
`SUCCESS`; all destinations visible; zero retransmissions and zero timeouts.

Additional pressure-shape checks:

```text
results/spec164-t027-prefix-ready-r1c16-30s-20260730T0920Z
results/spec164-t027-prefix-ready-r3c4-30s-20260730T0930Z
```

- r1/c16: admissible, 16 cold destinations, original `NO_ROUTE` absent;
  16 MiB retrieved in 10.405 seconds.
- r3/c4: admissible, all 12 cold workers `SUCCESS`, all destinations visible,
  zero retransmissions and zero timeouts.

The earlier r1/c16 quick-smoke 15/16 result was caused by the harness's
documented eight-second quick-smoke clamp; the identical 30-second bounded run
completed. The permanent regression therefore uses a 30-second operation
deadline and the formal 60-second measurement-window declaration, without
changing any transport retry or resource bound.

## Verification

```text
python3 -m unittest discover -s tests/python -p 'test_spec164_*.py' -v
Ran 101 tests in 4.381s — OK

examples/run_spec164_file_producer_registration_regression.sh
SPEC164_FILE_PRODUCER_REGISTRATION_REGRESSION_OK
```

The Python extension was rebuilt from the changed C++ source with low-memory
compiler flags before these real-seam runs. T028 must now execute a new frozen
matched campaign; none of the earlier frozen evidence is modified.
