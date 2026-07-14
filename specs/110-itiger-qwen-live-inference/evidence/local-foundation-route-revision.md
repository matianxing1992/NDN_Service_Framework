# Local foundation route revision

**Status**: implementation/static gate PASS; full local foundation build and
registry publication remain open under T166.

## Why the route changed

The two preserved GitHub identities failed at different stable dependency
layers: run `29283330479` at NDNSD dependency ordering and run `29285142648` at
NFD configuration. A second local build-graph audit then found independent
OpenABE/RELIC adapter defects, an incomplete Python dependency closure, missing
runtime library/config handling, and assumptions that waf programs declared
with `install_path=None` would be installed. Repeating the entire stable C/C++
build in GitHub made each cloud identity an expensive discovery mechanism.

## Revised ownership

| Layer | Owner | Acceptance boundary |
|---|---|---|
| NFD, ndn-cxx, ndn-svs, NDNSD, OpenABE/RELIC, NAC-ABE, NDNSF core | local `Dockerfile.foundation` | sealed source, exact revisions, compile/install/tests, local runtime probes |
| ONNX Runtime GPU SDK, PyTorch/CUDA Python closure, native ONNX provider adapter | dispatch-only GitHub `Dockerfile.gpu` | foundation digest input, ONNX asset SHA-256, build/link/import/closure gates |
| CUDA device/provider and Qwen distributed inference | iTiger Slurm + Apptainer `--nv` | allocated GPU correlation and real execution evidence |

GitHub has no GPU and cannot satisfy the final GPU gate. Qwen weights remain
under iTiger `/project` and never enter either OCI build context.

GHCR package visibility is an operator precondition. The previous workflow
attempted an unsupported Packages REST `PATCH` after publishing. That mutation
is removed: the package must be made public in GitHub Package settings once,
and every candidate now proves anonymous digest access with an empty Docker
credential directory before its release manifest may be accepted.
See GitHub's official [package visibility
procedure](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility).

## Locked repair inputs

- `libpcap-dev` remains required for full NFD 24.07 capability.
- The known-good local installation is Ubuntu 20.04.3 with system OpenSSL
  1.1.1f. `/usr/local/lib/libopenabe.so` and `libnac-abe.so` resolve to
  `libssl.so.1.1`/`libcrypto.so.1.1`; NAC-ABE's CMake cache records OpenSSL
  v1.1.1f. There is no measured OpenSSL 1.2 dependency: the accepted ABI is
  specifically OpenSSL 1.1. Foundation and CUDA bases are therefore pinned to
  Ubuntu 20.04.
- PyTorch 2.6 and Transformers 4.48 have no Python 3.8 wheel. Python 3.10.18 is
  copied from the digest-pinned official Bullseye slim image; both bases use
  glibc 2.31, and no PPA or host Python state enters the image. The copied
  interpreter was exercised on the pinned Ubuntu 20.04 base and loaded SSL,
  readline, SQLite, compression, hashing, decimal, UUID, and ctypes using
  OpenSSL 1.1.1f. Its Focal runtime closure is explicitly locked to `libgdbm6`,
  `libreadline8`, `libsqlite3-0`, and `libssl1.1`; unused Bullseye-only NIS and
  Tk extensions are removed instead of importing foreign system libraries.
- NFD websocketpp gitlink: `ac4e021333675fc80b96eb7be45d218581c897e2`.
- OpenABE RELIC revision: `b984e901ba78c83ea4093ea96addd13628c8c2d0`.
- ONNX Runtime GPU C++ 1.20.1 Linux x64 archive: 258487100 bytes,
  SHA-256 `6bfb87c6ebe55367a94509b8ef062239e188dccf8d5caac8d6909b2344893bf0`.
- Docker Registry config blobs for both pinned CUDA build and runtime digests
  report CUDA 12.4.1, NCCL 2.21.5-1, and cuDNN 9.1.0.70-1. NCCL is therefore a
  measured base-image input for distributed inference, not an assumed or
  silently omitted PyPI dependency.

## Static evidence

The revised unit suite passes 14/14 cases. It verifies local-only sealed source
consumption, dependency order, NFD inputs, OpenABE adapter, ONNX asset identity,
Python closure, explicit installation of `App_ServiceController` and
`di-native-provider`, runtime closure, Qwen exclusions, dispatch-only workflow,
transient-seal Git hygiene, and failure evidence retention. The release pipeline shell integration passes,
the lock parses as JSON, Python scripts compile, shellcheck passes, and Docker
successfully parses/builds the external-foundation stage.

After the Boost 1.71 compatibility correction, the complete Spec 110 offline
suite passes 97/97, release-pipeline integration passes, and the four changed
source/document files scan with zero secret findings across 35039 bytes.

These checks do not close T166. The next admissible state transition is a new
committed source identity, matching seal, complete local foundation build, and
one source-bound foundation publication. Only then may T167 dispatch one GPU
assembly.

The isolated OpenABE gate was then executed against the pinned Ubuntu 20.04
base. Its complete upstream test suite and CLI encryption/decryption regression
passed, the image completed in 191.2 seconds, and `libopenabe.so` resolved
`libssl.so.1.1`, `libcrypto.so.1.1`, `librelic.so`, and `librelic_ec.so` with no
missing library when the release library path was active. The same fixture on
Ubuntu 22.04/OpenSSL 3 had first produced a compile incompatibility and then nine
cryptographic/DRBG test failures; that route is rejected rather than patched or
explained away.

NAC-ABE derives from commit `1cc17d9d21f4dfc0921cc77315d0c57d46291880`, which discovers system
OpenSSL and `libopenabe` through CMake and has no independent bundled OpenSSL.
Its upstream CMake registers tests only below the `tests/` subdirectory, so a
root-level `ctest` can misleadingly report no tests. The foundation gate instead
builds with `-DHAVE_TESTS=TRUE` and executes the unit-test binary from the
directory containing `trust-schema.conf`; missing test configuration is a hard
failure, not an accepted crypto result.

The user's modified NAC-ABE worktree executed 29 cases in 63.44 seconds and
ended with `*** No errors detected`, but one concurrency case is an uncommitted
local research change and is not counted as candidate evidence. The exact
locked source has 28 cases covering CP/KP setup, key generation,
encryption/decryption, integrated producer/consumer flows, retries,
1000-operation encryption/decryption benchmarks, and 1000/1000 successful
cached decryptions. An earlier invocation from the wrong directory produced 18
missing-`trust-schema.conf` failures; it is retained as a test-launch error and
was not counted as cryptographic evidence.

## First complete-build attempt and ndn-svs compatibility correction

Committed candidate `001b48b4f7d2fded1480c0d59c6482a51ea2edbd` was sealed as
`sha256:e8e181eb251ef1fee36c909715263a4d9f4dfa2895aca5255e29e1950594d78d`
and started the first complete Foundation build. Ubuntu 20.04 installed the
expected Boost 1.71 closure, ndn-cxx configured against OpenSSL 1.1.1f and
compiled all 181 build actions, but the locked ndn-svs revision stopped at a
Boost 1.74 version check. The complete negative log is retained at
`results/spec110-itiger-qwen-live/foundation-build/build.log`.

Repository history proves that check came from upstream-oriented commit
`e0c436e` and replaced the project's earlier Boost 1.71 gate; no corresponding
Boost-1.74-only code dependency was introduced. The rejected correction that
would have embedded a separate 109 MB Boost 1.74 source build was committed as
`baa000d` and explicitly reverted by `5f1e794` before any new seal or Foundation
publication. It was never pushed as an NDNSF release candidate.

The project-compatible ndn-svs correction changes only the configure threshold
back to 1.71 while retaining the ndn-cxx 0.9 requirement. Host configuration
then passed with Boost 1.71.0 and ndn-cxx 0.9.0. The first dependency correction
was `19ec38ec77d26c13125b292863e607da51a3d9de`, published on the fork branch
`spec110-boost171`.
Because the dependency lock and workspace changed, the failed candidate's seal
is retired and the next full build requires a new commit-bound seal.

Committed candidate `5dd91c2584d78b2bc516a5babc2b8d340f2c0446` was sealed as
`sha256:27b59bae5134c87eefc9d9c27db9e46530ca0f352c942d5f49722f2eccfada01`.
Its full build confirmed Boost 1.71 across ndn-cxx, ndn-svs, NDNSD, and NFD;
NFD also compiled with the locked libpcap and WebSocket++ inputs. The next
stable failure occurred when OpenABE compiled its upstream test binary:
`gtest/gtest.h` was absent because `NO_DEPS=1` intentionally disables
OpenABE's dependency installer but the Foundation system-package closure had
omitted `libgtest-dev`. The complete negative log is retained at
`results/spec110-itiger-qwen-live/foundation-build-5dd91c2/build.log`.
The isolated passing OpenABE fixture had explicitly installed
`libgtest-dev`, so the correction adds that Focal package to the locked system
closure and makes preflight/unit tests reject any future omission. No OpenABE,
RELIC, OpenSSL, or test source is bypassed.

Candidate `fce4c3a73c674df161e560978969cac9b6146f65`, sealed as
`sha256:3dac110945d38e8c133e0ce5be16451dd6b8d8545ec3215f243a3eaddd7d443d`,
then completed ndn-cxx, ndn-svs, NDNSD, NFD, and all 46 OpenABE tests. Its
NAC-ABE test executable failed to link because the locked CMake target used
`boost::filesystem` without declaring the filesystem/system DSOs. The negative
log is retained at
`results/spec110-itiger-qwen-live/foundation-build-fce4c3a/build.log`.
The dependency-only fix explicitly links `Boost::unit_test_framework`,
`Boost::filesystem`, and `Boost::system`; with CMake forced to the system Boost
1.71 configuration, all 28 locked tests passed in 67.03 seconds. That exact fix
is commit `390e9001a8611e04c90f3a5866d09c3136c885d0` on fork branch
`spec110-explicit-boost-test-link`, and preflight now rejects another NAC-ABE
revision.

Candidate `a69799c2b257469d3deb0105bcab70dfcdb91414`, sealed as
`sha256:69a2c1eea94021f5a59bb4082286c83f3eae456b9ab536a127e8132251d69a42`,
passed the complete NDN, OpenABE, and NAC-ABE layers. NDNSF core compilation
then proved that the dependency seal still omitted 838 lines of local ndn-svs
publication-fetch, parallel-fetch, repair, and retry work on which
`ServiceUser` and `ServiceProvider` already depend. The complete negative log
is retained at
`results/spec110-itiger-qwen-live/foundation-build-a69799c/build.log`.

The original dirty ndn-svs worktree was left untouched. Its exact runtime diff
was applied to an isolated worktree on top of the Boost 1.71 correction. Three
test-only compatibility defects were repaired: a complete Asio include,
explicit conversion from ndn/Boost milliseconds to `std::chrono`, and
asynchronous Interest isolation including disabling inner retries in the
outer-backoff test. The library and all 29 ndn-svs tests then passed under Boost
1.71. The sealed dependency is now
`7b616b08624a79617bb05f2d3553bbbacdc4c482`, published on branch
`spec110-runtime-publication-fetch`; `gpu.lock` and preflight reject any other
ndn-svs revision.

## Audit verdict

**PASS for the reviewed build graph; T166 remains open.** No unresolved
BLOCK/HIGH issue remains in the static/local route after the OpenSSL ABI,
NAC-ABE test-launch, Python/Focal closure, NCCL base-image, GHCR anonymous-pull,
runtime-library, and sealed-source gates above. This is not a foundation release
claim: the complete committed/sealed `Dockerfile.foundation` build, in-container
probe, source-bound push, and immutable digest record still belong to T166.
The first full build was started and preserved as the ndn-svs configure failure
above. Useful Docker build cache was retained for the corrected candidate;
T166 remains open until that candidate passes every in-container gate and is
published by immutable digest.
