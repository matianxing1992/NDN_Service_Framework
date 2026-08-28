# Build And Unit Validation

## Reconstructed Pre-Fix Baseline

**Date**: 2026-07-15

```bash
./waf configure --with-tests
./waf -j4
```

Result: PASS. Configuration detected Boost `1.71.0`, ndn-cxx `0.9.0`, and g++
`9.4.0`; all 17 build steps completed. The compiler emitted the existing
project warning that GCC older than 10.2 is not officially supported, but no
compile or link failure occurred.

Pre-fix artifact hashes:

| Artifact | SHA-256 |
|---|---|
| `build/libndn-svs.so` | `7965917f8e9dbce25a0d4009807f9ee8846f948011a519e0ebd103f1d37da3d5` |
| `build/unit-tests` | `b4ea1e2128ac58489bb22087ea5b020ccb64f593c833ad07f78f2df2e76da8b1` |

The worktree diff identity at this checkpoint was
`0eeccda65f0d46a3cb7f64fd780653faff4525e5d14bc7bfee194f3f6e367f58`.
Final committed-source hashes and complete-suite counts will be appended after
T036-T043; these baseline hashes are not final acceptance evidence.

## Post-Fix Complete Suite

```bash
./waf -j4
git diff --check
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests --log_level=test_suite
```

Result: PASS, 42/42 test cases, `*** No errors detected`. The run includes 11
Core, 1 MappingProvider, 1 SecurityOptions, 21 SVSPubSub, and 8 VersionVector
cases. `ldd` confirmed the test process loaded
`/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`.

Post-fix uncommitted artifact hashes:

| Artifact | SHA-256 |
|---|---|
| `build/libndn-svs.so` | `a64436b80c0fb7c8ce49e947b02b6e83fe7a0661d39b9a45a415d74e1843ba43` |
| `build/unit-tests` | `7092fff7096a7c6281fda0bb39563c36a6de24f41cce6ff980064fe88f200f7c` |

Final committed-source rebuild remains required by T043; these hashes prove the
passing review surface but are not yet the final candidate identity.

## Final Committed-Source Rebuild

From clean HEAD `c34c04d766836bba1567a70bae846dfbd9d25b66`, `./waf
clean`, configure, 17-step build, rebuilt-library `ldd` verification, and the
complete test command passed. Result: 42/42. Reproducible artifact hashes equal
the post-fix snapshot hashes above, proving commit reconstruction did not alter
the tested product tree.
