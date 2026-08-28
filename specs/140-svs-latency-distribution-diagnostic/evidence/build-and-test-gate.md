# Spec 140 Build And Test Gate

**Status**: `PASS`

## Focused Tests

```text
python3 tests/python/test_spec140_svs_latency_distribution.py -v
Ran 7 tests in 0.002s
OK
```

The tests cover known-sample mean/p50/p95/p99, empty input, rejection of a
legacy p99-only summary, count/statistic mismatch, correct concatenated-peer
percentiles, exact two-cell matrix, and frozen-root exclusion.

## Build

```text
python3 Experiments/build_svs_latency_distribution.py build
python3 Experiments/build_svs_latency_distribution.py verify
SPEC140_BUILD_VERIFY_OK
```

```text
binary sha256:
a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a

libndn-svs sha256:
7945f22bcdaaef149f4e3cc2a75a39d124e4dfd54d07027cadcc83b4f5b1308f

compiler:
g++ (Ubuntu 9.4.0-1ubuntu1~20.04.2) 9.4.0

CPU affinity captured by builder:
[0, 1, 2, 3]
```

Resolved Boost libraries are version 1.71.0. The binary resolves
`libndn-svs.so.0.1.0` from `/home/tianxing/NDN/ndn-svs/build/`.

## Frozen Evidence Guard

Before the new diagnostic, the frozen Spec 136 formal tree hash is:

```text
b074aaa7afc8f0a3fd3c35eb9dcd2fa8e70eb93ee72579d49191f4c230a996f1
```

This hash will be checked again after the Spec 140 diagnostic.

## Evidence Classification

- Metric capture: `implemented`
- Contract tests: `executed`
- New binary/linkage identity: `executed`
- Two-cell MiniNDN distribution: `not yet executed`
