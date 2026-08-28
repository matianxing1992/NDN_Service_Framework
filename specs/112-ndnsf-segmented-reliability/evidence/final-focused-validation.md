# Final Focused Validation Before Candidate Freeze

Date: 2026-07-15 (America/Chicago)

All source-affecting work for the five reported defects was complete before
this suite. Network opt-in tests were not rerun here because their immutable
focused cells are already recorded; the final network matrix receives a new
candidate after this gate.

## Native Suites

```text
../ndn-svs/build/unit-tests \
  --run_test=TestSVSPubSub --log_level=test_suite
```

Result: 16/16 passed, including final signed inner/outer packet sizing,
preparation/storage/Face failure rollback, sequence visibility, malformed
segments, validation failure, shutdown, and callback lifetime.

```text
./build/unit-tests \
  --run_test=GenericDynamicApi/TargetedInvocation --log_level=test_suite
```

Result: 18/18 passed, including Python-relevant registration/token behavior and
the new total-deadline/exactly-once terminal cases.

```text
SPEC112_LIFECYCLE_ROLE=User \
../NAC-ABE/build-tests/tests/unit-tests \
  --run_test=TestAbeSupport/ProcessLifecycleProbe \
  --log_level=test_suite --report_level=no
```

Result: passed after CP-ABE setup, key generation, encryption, decryption, and
normal process exit. The full subprocess gate separately passed 300/300 exits.

## Python Suites

Executed sequentially with `PYTHONPATH=pythonWrapper` where required:

- `test_spec112_candidate_manifest.py`: 6/6 passed;
- `test_spec112_segmented_response_tools.py`: 5/5 passed;
- `test_spec112_segmented_response_campaign.py`: 5/5 passed;
- `test_spec112_segmented_response.py`: compiled-extension check passed; the
  exclusive MiniNDN case was intentionally skipped here;
- `test_ndnsf_targeted_python_api.py`: 3/3 non-network checks passed; exclusive
  MiniNDN case intentionally skipped;
- `test_spec112_targeted_timeout.py`: 2/2 non-network checks passed; exclusive
  MiniNDN case intentionally skipped;
- `test_spec112_nac_abe_exit.py`: one three-role lifecycle cycle passed 3/3.

The option-specific MiniNDN evidence already passed under
`spec112-2cf00e943453690ea2f2`, `spec112-796140a912f13c323020`, and
`spec112-7d6471b217bc3bdd4efe`. Those candidates remain evidence history, not the
final integrated candidate.

## Artifact Hashes

| Artifact | SHA-256 |
|---|---|
| `../ndn-svs/build/unit-tests` | `2fb8b465744beab4d47d45de2ba43a718deba01cebd93f7dd3ecba2b0db58ac9` |
| `../ndn-svs/build/libndn-svs.so` | `491d563a17f4c5c4687f928b6bb7f3fbd6ff63aa86d9e4b6dad46ba9736d0da9` |
| `build/unit-tests` | `b90e3c872dcb478cb2a673868ab1d0e6faea0b9a7cd70bc6f51f1c6b92544fec` |
| `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so` | `0fa7c6e7b8a235a15666fd0bfe7fbfa7d488f47024162b7ca7347935652a25f9` |
| `../NAC-ABE/build-tests/tests/unit-tests` | `90bdc8ff8eecc63442a84c09fce35c383447ac947f95387109475d27ecdfb3d9` |
| `../NAC-ABE/build-tests/libnac-abe.so` | `40e5575d4baea3cf49c2c98e9a9aaafbe69e2d10f3d1ef3dec483d4b06e256ea` |

`git diff --check` passed in both the framework and NAC-ABE repositories.
The candidate manifest regression now binds both the rebuilt Python extension
and the actual NAC `build-tests` lifecycle binary.

## Pre-Completion Convergence Rerun

The pre-completion audit found one wrapper-lifetime gap after the first final
candidate: the synchronous Targeted wrapper's local fallback could return while
native callbacks still held stack references. T048 changed only that adapter to
copy submission inputs and retain a shared, atomically claimed terminal state.

Executed after rebuilding the in-place Python extension:

```text
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation --log_level=test_suite
PYTHONPATH=pythonWrapper python3 tests/python/test_spec112_targeted_timeout.py -v
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_targeted_python_api.py -v
```

Results: C++ Targeted 18/18; Targeted-timeout non-network checks 3/3 with one
exclusive MiniNDN test skipped by design; Python Targeted API non-network checks
3/3 with one exclusive MiniNDN test skipped by design. The rebuilt extension is
`a9e4423eb85104a4d2061f278d09115bc83c1dfeccf3848357a3aa5865470eb5`.
The new integrated candidate is `spec112-62b57fe47b2e3537ad23`; its six final
cells are recorded in the completion summary and passed exactly once.
