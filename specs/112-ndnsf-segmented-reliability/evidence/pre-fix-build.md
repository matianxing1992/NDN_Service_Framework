# Spec 112 Pre-Fix Build Evidence

## T007: Local Boost Baseline

**State**: implemented-only; configure/build/test execution is recorded below by
T008 and T009.

The local NDNSF/ndn-svs development environment is Ubuntu 20.04 with
`libboost-dev 1.71.0.0ubuntu2`. This project has intentionally used Boost 1.71;
the 1.74 wording is retained only when preparing upstream-facing ndn-svs
documentation. It is not evidence that this checkout requires 1.74 APIs.

Spec 112 therefore restores `../ndn-svs/wscript` to a 1.71 configure gate so
the actual current dirty source and tests can be rebuilt rather than accepting a
six-week-old test binary. This is a build-prerequisite correction only: it does
not change segmentation behavior or claim compatibility beyond the tested local
toolchain.

Observed before configure:

- Boost package: `1.71.0.0ubuntu2`
- ndn-cxx pkg-config: `0.9.0`
- compiler: `c++ (Ubuntu 9.4.0-1ubuntu1~20.04.2) 9.4.0`
- prior `../ndn-svs/build/unit-tests` SHA-256:
  `221bc508a94065cd4ba8ab5baa3f1a19040a9e6b09afafd6ec70e84e32bc56f3`

## T008: Matched Configure And Build

**Result**: PASS after continuing past two pre-existing test compilation errors.

Commands:

```bash
cd /home/tianxing/NDN/ndn-svs
./waf configure --with-tests
./waf build -j"$(nproc)"
```

Configure detected GCC 9.4.0, C++17, ndn-cxx 0.9.0, Boost 1.71.0, and
`boost_unit_test_framework`; it completed in 0.826 s. The first incremental
build exposed two compilation defects in previously dirty tests:

1. `svspubsub.t.cpp` added an ndn-cxx `time::milliseconds` (Boost chrono) to a
   `std::chrono::steady_clock` time point and used `tlv::RepairData` without
   including `tlv.hpp`. The duration is now explicitly converted to
   `std::chrono::milliseconds` and the defining header is included.
2. `mapping-provider.t.cpp` instantiated and called
   `boost::asio::io_context` with only ndn-cxx's forward declaration. It now
   includes `<boost/asio/io_context.hpp>`.

The build was continued after each fix; configure was not repeated. The final
incremental build completed successfully in 5.403 s. These test-only repairs
make the already-present tests compilable and do not alter SVS runtime behavior.

Recorded identities after the successful build:

| Item | SHA-256 |
|---|---|
| branch / HEAD | `Experimental` / `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` |
| complete tracked binary diff | `83bf02405c3d8bc299ec397e824695c04ccf792f59993a6893f3a57862f42686` |
| porcelain status | `51780db1abbe13004c9d9241768f04c23365a19aacf0fc7981146b4ef651f45a` |
| `wscript` | `04ddca7d2ffdba98a2d9974e9b7c2f18c1ef06b47410e48d63a0846e04b03846` |
| `build/unit-tests` | `148a93c7f245a1ce64384f02e83002eb3884f48e00e6f0f8a641ea89011e3c58` |
| resolved `build/libndn-svs.so` | `96b1266ceee38ab64b1f954bd3617f9ec9003bbf91cd275c0b15ab268213bf9b` |
| `svspubsub.t.cpp` | `c29d921fb6d9814d049cb69c6d16c60be0ebe945662a6cb956ccbf6562c21f53` |
| `mapping-provider.t.cpp` | `f7eb2e5e0b28f37ecd54bd3cc65ae31ab1e4ec8e09be0bbb89036a1d8c03aadb` |
| configure log | `fb87a180376a3ce2b996893e4ef5a2dd04accb15daf577e4939b436f0d35e9ca` |
| cumulative build log | `52f8476116020a014898609376fca48541487d8306cc5435befacddb53fb9f22` |

The test executable's default loader path resolves ndn-svs from `/usr/local`.
T009 therefore prepends `/home/tianxing/NDN/ndn-svs/build` to
`LD_LIBRARY_PATH`; `ldd` then resolves the just-built
`build/libndn-svs.so.0.1.0`. This prevents a false pass against the older
installed library without installing a pre-fix candidate system-wide.

Root filesystem after build: 189,110,804,480 B total and 21,072,998,400 B
available (89% used).

## T009: Existing TestSVSPubSub Suite

Executed once with the matched build library. The retained outcome is 6 passed
and 3 failed; see `pre-fix-unit.md`. No rerun was performed.
