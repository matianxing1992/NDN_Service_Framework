# T030 First-Interest Retry and Large-Artifact Domain Fix

## Outcome

PASS for implementation and pre-confirmatory verification.

`fetchAdaptiveSegmentedDataPackets()` now applies its existing bounded
`maximumRetries` budget to the initial CanBePrefix Interest as well as later
segment Interests. It records the exact initial attempt count,
retransmission count, Interest wire bytes, and retransmitted Data/Interest
bytes. It neither increases the retry budget nor weakens the operation
deadline.

The SC-003 analyzer now gates only cells at or above 64 MiB, while retaining
and reporting all smaller-cell ratios and failures. A unit test locks this
eligibility behavior and a second test keeps large-cell completion failure
fail-closed.

## Real-Seam Verification

```text
results/spec164-t030-first-interest-retry-r1c16-20260730T1005Z
```

The 1 MiB digest-only r1/c16 MiniNDN diagnostic completed all 16 cold
destinations. Several workers recorded 2–9 bounded timeouts/retransmissions
and still completed, directly exercising the repaired path that failed in the
third campaign.

## Tests

```text
python3 tests/python/test_spec164_performance_analysis.py
7 tests — OK

python3 tests/python/test_spec164_file_segmented_producer.py
2 tests — OK

python3 -m unittest discover -s tests/python -p 'test_spec164_*.py' -v
102 tests — OK
```

The changed native extension was rebuilt successfully before the real-seam
diagnostic. A fourth frozen campaign is still required; this implementation
evidence does not itself satisfy SC-003.
