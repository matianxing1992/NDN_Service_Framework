# T010/T011 negative verdict repair — 2026-09-05

Status: focused verdict/readiness repairs verified; final host build identity
verification recorded below. T014 remains BLOCK on the protected-grant gap.

## Reproduced defect

The maintained User exception handler converted arbitrary request exceptions
into the selected Y-N-C/P/R/E PASS marker. An unchanged-handler AST probe with
`UNRELATED_REPOSITORY_TIMEOUT` reproduced all four false positives. The
regression suite `tests/python/test_spec180_negative_verdict.py` initially
reported **12 failed**: unrelated exceptions, wrong lifecycle phase, an internal
strategy exception wrapped as infeasibility, and a fabricated epoch rejection
were all accepted.

The first repaired test attempt had three passes and nine fixture errors:
the isolated AST namespace omitted the production `RunnerError` name. The
fixture now supplies that exception type; this was a test namespace error,
not a live run or qualification result.

Y-N-E threw its own `DI_PROTECTION_EPOCH_REJECTED` without invoking a grant
verifier. The User also accepted any failed Response as Y-N-I evidence. Earlier
Y-N PASS labels therefore remain historical observations, not proof of the
registered safety boundary. A fresh result with corrected semantic validation
is required; the earlier raw runs and frozen records are preserved.

## Repair boundary

Require the production exception type, exact rejection, observed lifecycle
phase and request/attempt identity. Reject internal strategy failures even
when the coordinator wraps them in its no-feasible-candidate error. Only
Provider-side evidence can establish non-ingress input rejection. Report
Y-N-E as unavailable until a real grant mutation reaches the verifier.

Follow with strict runner record and child-exit validation, then current-source
build binding and the FR-008 grant path. No SIF/Tiger qualification is authorized
by the focused regression.

The User handler regression now passes **13/13**. Four new runner tests first
failed because the old marker equality check had no current-request, source
owner, lifecycle or sibling-crash validation. The runner now validates those
fields and checks every sibling before accepting a marker.

The combined User, runner, application and local-build guard suite subsequently
passed **153 tests**. One intervening run had 143 passes and one assertion
failure: stricter field validation preceded the intended owner error. Owner
validation now runs first; neither version accepted the invalid record.

The runner now requires User exit 91 for a negative or 0 for control, rejects
unrelated sibling exits, lingering children and forced-kill cleanup, and writes
PASS only after teardown succeeds. A host checkout verifies the actual Python
import, framework shared library, Provider executable and source/config hashes
before creating MiniNDN. Exact-SIF still uses its sealed runtime, not host builds.

## Unified-build diagnostic

The first unified local Waf invocation rebuilt the framework and Provider
successfully (68/68 targets, 1m55s), then rebuilt the Python extension. Inspecting
the framework showed its configured RUNPATH still selected the repository's
legacy dependency prefix, while NFD selected `/usr/local/lib/libndn-cxx.so.0.9.0`.
The stored configure command omitted the existing
`--disable-local-dependency-prefix` option. A directory named `build-system-j2`
does not prove system linkage. This is a build/configuration failure, not a
protocol observation; no live matrix was launched with that closure.

The correction is to reconfigure the same build with that option, retain its
explicit NDN-SVS source/build pair, rebuild the unified targets and extension,
then verify actual imports, library hashes and NFD linkage before the smallest
real-forwarder readiness check. No global installation or other NDN-SVS checkout
is modified.

The extension's independent `pkg-config libndn-svs` lookup also selected the
installed headers/library, not Waf's explicit source/build pair. They are
different bytes: installed library SHA-256
`2f258b7c41f357bb3754bc896e569aaac2744d0a307ab0d2787c8b95cd9bf211`,
selected library SHA-256
`9fe2bdc9bf5fe2f9f191dda1f11944a86a348ad2de118a95aa739e2c170844ee`.
Header hashes also differ. The fix must bind both headers and library for the
extension; a runtime-only override is insufficient. The in-progress first
extension build is obsolete and cannot authorize a manifest or a live run.
The owned compiler was deliberately terminated after that mismatch was
confirmed. Setup and the unified helper returned exit 1; this was a controlled
abort of the obsolete build, not an unexplained compiler/source failure.

The corrected configure passed in 12.963s and the framework/Provider relink
passed in 14.869s. `ldd` now resolves ndn-cxx and NAC-ABE from `/usr/local`,
with NDN-SVS from the declared development build; the framework RUNPATH has no
legacy dependency prefix. The first standalone readiness-test compile then
failed with `nac-abe/algo/master-key.hpp: common.hpp: No such file or directory`
(exit 1), before any test process ran. Its command lacked the NAC-ABE include
closure already present in Waf; correct the diagnostic command, not the library.
An attempted macro-only correction failed identically: pkg-config already
supplied `NAC_ABE_CMAKE_BUILD`. The actual missing argument was
`-I/usr/local/include/nac-abe`, required by the installed nested header's
unqualified `#include "common.hpp"`. Both failed compiles ran no test.
The next compile progressed to `NDNSFThreadPool.hpp` and exposed the second
missing include root, `-Indn-service-framework`. The standalone command now
declares both roots; this was another command closure error, not a test PASS.
The subsequent link used the ambient Linuxbrew linker and omitted NDNSD,
producing unresolved dependencies (exit 1). The diagnostic now uses the same
closed `/usr/bin/g++ -B/usr/bin` toolchain as Waf and links `ndnsd` plus `dl`.
No fixture or real NFD check had run at these compile/link failures.
The corrected standalone compile passed. Its first execution returned 0/11:
all cases stopped before readiness at `TPM signing failed` for the host's
pre-existing Controller identity. The test's in-main environment setup did not
isolate the initial native KeyChain state. Do not repair this by changing the
operator PIB/TPM: establish a private environment before process startup.
Launching with private PIB/TPM locators in the external process environment
passed that boundary, then all 11 cases rejected the fixture's zero-byte policy
file (`No such node (provider-policies)`). The fixture needs the real policy
schema's empty sections. These are test setup failures; no readiness PASS is
claimed from them and the operator's keys were not repaired or replaced.

The separate remaining security gap is detailed in
`t008-protected-grant-gap-20260905.md`; no amount of startup retries closes it.

After explicit SVS header/library binding and closed setup toolchain checks
were added, the combined maintained regression command passed **183 tests in
7.27s**:

```bash
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
python3 -m pytest -q tests/python/test_spec180_negative_verdict.py \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_spec180_yolo_application.py \
  tests/python/test_spec180_native_build.py --tb=short
```

The final local extension build explicitly uses `CFLAGS='-O0 -g0'` and
`CXXFLAGS='-O0 -g0'` to bound implementation-only glue compilation; Waf's cached
framework/Provider optimization remains unchanged. The manifest records these
effective flags. This build is not a release-performance or SIF qualification.

## Current focused readiness result

The mandatory external launcher sets private PIB/TPM/transport locators before
the NDN-SVS `DEFAULT_KEYCHAIN` static initializer loads. It checks the real
Parser's empty policy sections and preserves private logs. Direct invocation
without the launcher is rejected. The historical first test did touch the host:
`/test/controller-readiness` exists with public certificate/TPM/PIB timestamps
at 07:20:56 UTC. With no pre-run snapshot, not every prior record can be
attributed safely. It has been left intact rather than deleting an ambiguous
existing identity; no private-key content was printed or copied.

Commands and results:

```bash
python3 tests/standalone/run-service-controller-readiness.py \
  /tmp/ndnsf-controller-ready.ubyHl1/controller-readiness
# 11/11 PASS; /tmp/ndnsf-controller-readiness-4muig2bt/test.log

python3 tests/standalone/run-service-controller-readiness.py \
  /tmp/ndnsf-controller-ready.ubyHl1/controller-readiness --real-nfd
# PASS; /tmp/ndnsf-controller-readiness-gh3tzmoo/test.log and nfd.log
```

Real NFD forwarded two unique random PUBPARAMS probes from Faces 258/259 to
authority Face 257 and returned the signed Data to the respective probe Face.
Both requests arrived with HopLimit 1->0. NFD exited 0, with no forced kill.
This closes the focused startup path, not Y-A/Y-B/Y-N or T015 qualification.

SHA-256 bindings:

- Controller.cpp: `02f88c4a5322e142694e8766d42fddb4a89df83618033402ce49bab197c26261`
- Controller.hpp: `3976f24facd7b5a7d4c888043737348f6fa82c40edf331dba41f758960f5813a`
- test source: `194face8f7f7f2d45be6efe2b48257cce876b1a4b5551a7ecdfc551c409dda11`
- launcher: `651c5a6b2b3a2271edc035bb85a60dbc5db8d135844287438ccb5d6d2c131a9b`
- diagnostic binary: `e351d510cb7bab1a3d554d4a07402e1df99266521047896f8fc4e3b8cb12f61f`

The first actual runner-environment identity check rejected an additional
`libnss_files-2.31.so` mapping: removing HOME triggered a deferred uid lookup.
Both probes now explicitly perform that lookup, retaining full library/hash
comparison rather than ignoring a dependency. Probe subprocesses also receive
private keychain paths before DSO load. Two regressions were added; the combined
set passed **185 tests in 8.53s**. No full matrix was launched after the mismatch.
