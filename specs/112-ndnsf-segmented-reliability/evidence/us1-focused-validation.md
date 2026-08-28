# US1 Focused Validation — Segmented Responses And Oversize Abort

## Disposition

Email defects 1 and 2 are fixed in the current focused candidate. The former
fixed `8000`-byte content slice was replaced by fitting against the final signed
inner Data embedded in the final signed outer SVS Data. Packet preparation is
complete before storage and sequence advertisement; partial store writes are
rolled back, and asynchronous validation/callback paths use lifetime guards.

Evidence level: **executed-pass (unit + real compiled Python binding + 0% MiniNDN)**.
This is focused US1 evidence, not the integrated final-candidate claim required
by T037–T042.

## Candidate And Binary Identity

- Candidate: `spec112-2cf00e943453690ea2f2`
- Identity SHA-256: `2cf00e943453690ea2f2f3489bf46ba706ee7cbaa739dba93c592e02c196bf2e`
- Manifest: `results/spec112-segmented/spec112-2cf00e943453690ea2f2/candidate-manifest.json`
- Manifest SHA-256: `cffcedfb2b7a5611f0c119c6f90962888a6a932f0ec570f99af29c3182112cb6`
- ndn-svs build and installed library SHA-256:
  `491d563a17f4c5c4687f928b6bb7f3fbd6ff63aa86d9e4b6dad46ba9736d0da9`
- ndn-svs unit-test binary SHA-256:
  `2fb8b465744beab4d47d45de2ba43a718deba01cebd93f7dd3ecba2b0db58ac9`
- NDNSF library SHA-256:
  `b0199e56fd2d90a2484ae625e4e4b7d74c8112dd364df509283a99475638b40e`
- Compiled `_ndnsf` extension SHA-256:
  `cdf95ea389632bd71f62b3a05a9ad510226a2c2e20b66a0d8e08ef6ad24df22b`
- `ldd` resolved `_ndnsf` through the rebuilt NDNSF library to
  `/usr/local/lib/libndn-svs.so.0.1.0`; installed and build ndn-svs hashes match.

## Test-First And Unit Evidence

The first build after adding T013–T015 failed because
`isFinalPacketSizeAllowed` and `decodeInnerSegments` did not exist. This was the
expected TDD red state. After implementation:

```text
LD_LIBRARY_PATH=$PWD/build ./build/unit-tests --run_test=TestSVSPubSub
Running 16 test cases...
*** No errors detected
```

The suite covers:

- 8799/8800/8801-byte final-packet acceptance boundary;
- 6,500, 7,999, 8,000, 8,001, and 16,000 application bytes;
- short and long application names plus a long certificate identity;
- exact reconstruction and no empty trailing segment;
- injected signing, storage, and `Face::put` failures;
- no visible sequence advance, rollback, and a healthy next publication at
  sequence 1 after each injected failure;
- missing, malformed, and duplicate inner segments;
- validation failure cleanup, idempotent late cleanup, callback after Provider
  destruction, and stored-but-unadvertised shutdown rollback.

Three pre-fix fixture failures were corrected without changing protocol
behavior: DummyClientFace output requires advancing its event loop, construction
traffic must be drained before counting operation-specific Interests, and the
adaptive outer-retry test must set `publicationFetchInnerRetries=0` to avoid
measuring the same inner retry round.

## Real Compiled-Binding MiniNDN Evidence

Command shape:

```text
sudo -n env \
  PYTHONPATH=$PWD/pythonWrapper \
  SPEC112_RUN_MININDN=1 \
  SPEC112_CANDIDATE_MANIFEST=$PWD/results/spec112-segmented/spec112-2cf00e943453690ea2f2/candidate-manifest.json \
  python3 tests/python/test_spec112_segmented_response.py -v
```

All four focused cells passed exactly once for this candidate:

| Cell | Result | Byte-exact | Provider alive | No reference |
|---|---:|---:|---:|---:|
| `focused-async-normal` | SUCCESS | 6/6 | yes | verified |
| `focused-async-targeted` | SUCCESS | 6/6 | yes | verified |
| `focused-sync-normal` | SUCCESS | 6/6 | yes | verified |
| `focused-sync-targeted` | SUCCESS | 6/6 | yes | verified |

Each cell used `64,4000,5000,6500,8000,16000`. Aggregate: 24/24 exact,
0 failures, 4/4 accepted cells. Observed end-to-end latency across these focused
requests was 17.543–146.627 ms; the four 8-KB requests were 44.400–90.464 ms,
and the four 16-KB requests were 58.436–106.738 ms. These are focused local
MiniNDN measurements, not a general performance claim.

- Campaign summary:
  `results/spec112-segmented/spec112-2cf00e943453690ea2f2/campaign-summary.json`
  (`f41332bf70cc96814303311cf4f3f4909c14e9fce28a9dd8881e4fd6d13e1912`)
- Campaign CSV:
  `results/spec112-segmented/spec112-2cf00e943453690ea2f2/campaign-cells.csv`
  (`af384bf743e3b7350f28fa0f97675540f525b5de71f708210d1180c90468a65d`)

## Preserved Diagnostic Candidate

`spec112-8e46b94d14627a39773f/focused-async-normal` is intentionally retained.
Its six requests were all byte-exact and the Provider stayed alive, but the
harness incorrectly required a numeric Provider exit code even while the
Provider was still running. The immutable cell therefore records FAILURE. The
harness was corrected and a new candidate identity was created; the old cell
was not overwritten or rerun.

## Residual Boundary

This phase proves only email defects 1 and 2. The burst/continued-health final
cell, degraded-Provider timeout cell, Python Targeted token decision gate, and
OpenABE lifecycle campaign remain assigned to later Spec 112 tasks.
