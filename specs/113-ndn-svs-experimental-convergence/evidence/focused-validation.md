# Focused Validation

## Reconstructed Mapping And Recovery Baseline

**Date**: 2026-07-15  
**Source baseline**: review worktree over `db9fc25`

Executed after a successful reconstructed-source build with the rebuilt library
forced ahead of the installed copy:

```bash
export LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./build/unit-tests --run_test=TestMappingProvider --log_level=test_suite
./build/unit-tests --run_test=TestSVSPubSub/MappingFetchSuppressesDuplicateInFlightRange --log_level=test_suite
./build/unit-tests --run_test=TestSVSPubSub/MappingFetchBackoffSuppressesRetryAfterTimeout --log_level=test_suite
./build/unit-tests --run_test=TestSVSPubSub/PublicationFetchTimeoutBacksOffAndIncreasesLifetime --log_level=test_suite
./build/unit-tests --run_test=TestSVSPubSub/RepairRequestRepiggybacksProducerData --log_level=test_suite
```

Result: 5/5 selected invocations passed with `*** No errors detected`.
The backoff test took about 2.56 seconds; the others took under 0.15 seconds.

An earlier combined Boost.Test filter returned exit 200 because this Boost 1.71
runner did not accept that combined expression. It executed zero tests and is
classified as a command-filter error, not a product failure. The five explicit
invocations above are the admissible focused result.

An initial invocation without `LD_LIBRARY_PATH` resolved
`libndn-svs.so.0.1.0` from `/usr/local/lib`. It is retained as diagnostic output
but excluded from evidence. All reported passing results above and below resolve
the library from `/home/tianxing/NDN/ndn-svs/build`.

The retained tests cover sparse mapping range results, duplicate in-flight
suppression, timeout backoff, adaptive publication fetch lifetime, and producer
repair re-piggyback behavior. No performance improvement is claimed.

## Transaction, Boundary, And Lifetime Tests

The following new/fixed behaviors were exercised individually before the full
suite:

- commit-head injection leaves visible sequence 0 and two prepared transactions,
  then ordered retry commits 1 and 2 and empties prepared/staged state;
- persistent commit failure leaves stored data while blocked and reclaims it on
  destruction;
- active `Face::put` is not called before the event loop and its injected
  failure does not prevent stored publication or sequence progress;
- direct and batched post-version-vector Sync-send failures remain non-throwing
  while local versions 1 and 3 remain committed;
- an unsupported rollback store rejects a single-packet asynchronous publish
  before insert; partial second insert erases the exact first name;
- actual signed outer Data objects were constructed at 8799, 8800, and 8801
  bytes; 8799/8800 were allowed and 8801 rejected by the limit contract;
- late Data, Nack, timeout, validation failure, and scheduled retry callbacks
  after Fetcher destruction produced zero terminal callback and zero retry.

All focused invocations passed from the rebuilt local library.
