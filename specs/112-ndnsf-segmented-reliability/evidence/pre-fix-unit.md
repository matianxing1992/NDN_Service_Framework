# Spec 112 Pre-Fix ndn-svs Unit Evidence

**Task**: T009  
**Execution policy**: one run for this exact pre-fix source/binary identity  
**Outcome**: executed-fail; retained without rerun

## Command

```bash
cd /home/tianxing/NDN/ndn-svs
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests \
  --run_test='TestSVSPubSub/*' \
  --log_level=test_suite
```

The loader resolved `libndn-svs.so.0.1.0` to
`/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`; it did not use the
older installed library.

## Result

- Process exit: `201`
- Test cases: 9
- Passed: 6
- Failed: 3
- Segmentation boundary tests: none existed in this pre-fix suite
- Provider crash: not exercised by these unit cases

Passed cases:

1. `LatePiggyDataSatisfiesPendingFetch`
2. `AsyncPublishQueuesWithoutImmediateFacePut`
3. `AsyncPublishNameOnlyAndPacketAreQueuedAndCommitted`
4. `LatePiggyDataStillSatisfiesPendingFetch`
5. `SmallDataIsPiggybackedAcrossMultipleRounds`
6. `RepairRequestRepiggybacksProducerData`

Failed cases, preserved exactly:

| Test | Assertion |
|---|---|
| `MappingFetchSuppressesDuplicateInFlightRange` | `face.sentInterests.size() == 1` failed: `0 != 1` |
| `MappingFetchBackoffSuppressesRetryAfterTimeout` | `face.sentInterests.size() == 1` failed: `0 != 1` |
| `PublicationFetchTimeoutBacksOffAndIncreasesLifetime` | `face.sentInterests.size() == 1` failed: `0 != 1` |

These are current pre-existing mapping/publication-fetch test failures, not
evidence for or against the email's segmented-response failure. They remain a
focused-suite blocker for SC-007 and must be diagnosed before T020/T037; they
are not rerun under this pre-fix candidate.

## Identities

| Artifact | SHA-256 |
|---|---|
| `build/unit-tests` | `148a93c7f245a1ce64384f02e83002eb3884f48e00e6f0f8a641ea89011e3c58` |
| `build/libndn-svs.so` | `96b1266ceee38ab64b1f954bd3617f9ec9003bbf91cd275c0b15ab268213bf9b` |
| retained log `/tmp/spec112-pre-fix-svspubsub-unit.log` | `6bce2496da20703fe163c7cc25808986f0e7b84bea8fb430b10e692af6d30b0a` |

The temporary log is transcribed completely above for durable tracked
evidence; `/tmp` itself is not treated as permanent storage.
