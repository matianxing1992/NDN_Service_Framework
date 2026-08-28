# Cross-Repository Integration Validation

**Date**: 2026-07-15  
**NDN-SVS HEAD**: `c34c04d766836bba1567a70bae846dfbd9d25b66`

## Candidate Linking And Rebuild

The system `/usr/local` installation was not overwritten. All commands used:

```bash
export LD_LIBRARY_PATH=/home/tianxing/NDN/ndn-svs/build:$LD_LIBRARY_PATH
```

`ldd` proved both NDNSF `build/unit-tests` and the rebuilt Python extension bind
to `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`.

Rebuild commands:

```bash
./waf -j4 --targets=unit-tests
cd pythonWrapper
python3 setup.py build_ext --inplace --force
```

The C++ target passed. The Python extension compiled and linked successfully.
One initial `python3 setup.py` invocation from the repository root returned exit
2 because that path has no `setup.py`; it performed no build and is excluded.
The corrected explicit `pythonWrapper` invocation passed.

| Artifact | SHA-256 |
|---|---|
| NDN-SVS `build/libndn-svs.so` | `a64436b80c0fb7c8ce49e947b02b6e83fe7a0661d39b9a45a415d74e1843ba43` |
| NDNSF `build/unit-tests` | `b90e3c872dcb478cb2a673868ab1d0e6faea0b9a7cd70bc6f51f1c6b92544fec` |
| `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so` | `a9e4423eb85104a4d2061f278d09115bc83c1dfeccf3848357a3aa5865470eb5` |

## Focused Results

```bash
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation
python3 tests/python/test_spec112_targeted_timeout.py -v
python3 tests/python/test_ndnsf_targeted_python_api.py -v
python3 tests/python/test_spec112_segmented_response.py -v
```

- C++ TargetedInvocation: 18/18 passed.
- Targeted timeout Python contract: 3 passed, 1 MiniNDN-exclusive skipped.
- Targeted Python API: 3 passed, 1 MiniNDN-exclusive skipped.
- Segmented response compiled-binding contract: 1 passed, 1
  MiniNDN-exclusive skipped.

The three skips are intentional and are executed only under the immutable
MiniNDN candidate in T047-T049; they are not counted as integration passes.

## Final Candidate Rebuild Correction

The first cross-repository build above linked the new NDN-SVS shared library,
but Waf compiled NDNSF objects against stale NDN-SVS headers installed under
`/usr/local/include`. This produced an ABI/object-layout mismatch: the first
formal MiniNDN candidate blocked in `SVSyncCore::setMaxSuppressionTime`, and a
GDB diagnostic showed the main thread waiting on a mutex without a genuine
runtime owner. A second clean build reproduced the failure because
`pkg-config --cflags libndn-svs` still selected the old installation.

The final committed NDN-SVS headers and library were installed, then the
affected NDNSF targets and Python extension were rebuilt from clean state:

```bash
cd /home/tianxing/NDN/ndn-svs
sudo -n ./waf install
sudo -n ldconfig

cd /home/tianxing/NDN/ndn-service-framework
./waf clean
./waf configure --with-examples --with-tests
./waf -j4 --targets=unit-tests
./waf -j4 --targets=ndn-service-framework
cd pythonWrapper
python3 setup.py build_ext --inplace --force
```

All four installed/workspace NDN-SVS header pairs (`core.hpp`,
`svspubsub.hpp`, `fetcher.hpp`, `store.hpp`) and the library pair now have
equal hashes. The candidate manifest rejects any future mismatch. Final
artifacts used by the passing candidate are:

| Artifact | SHA-256 |
|---|---|
| NDN-SVS workspace and installed library | `a64436b80c0fb7c8ce49e947b02b6e83fe7a0661d39b9a45a415d74e1843ba43` |
| NDNSF `build/unit-tests` | `6fba3a35a5ae4426503065bffc7fe8e668230c4a90b62ca54eaa1fb08cd6442f` |
| NDNSF `build/libndn-service-framework.so` | `6ad894435718177223e969cc6c739a8725a374eba33563b333f26d9e919792dd` |
| Python `_ndnsf` extension | `c065fa49a38e206420bcbf4b3025aff60be12270c911800489a942e9e09c2c15` |

The clean targeted root build passed 66/66 tests; the shared-library target
passed 17/17 build steps; C++ TargetedInvocation again passed 18/18; and the
focused Python tests retained only the three intentional MiniNDN-exclusive
skips. `ldd` confirms the Python extension loads the rebuilt root shared
library and workspace NDN-SVS library.
