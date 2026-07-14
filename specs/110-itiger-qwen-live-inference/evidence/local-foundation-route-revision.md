# Local foundation route revision

**Status**: T166 PASS; implementation/static gate, full local Foundation build,
immutable publication, and anonymous digest access are complete.

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

## Accepted local Foundation candidate

Committed workspace revision
`edeeff1e3041e941b48ba18784348fc9505d7418` was sealed as
`sha256:8472d030a337b6b978588038086db4ebdb57baee37827f0fcdc8b932d97eef0d`.
The seal binds lock digest
`sha256:0b617bb8a463734c837422786178e455abc327150d6cdbd0eccff16b6460312a`,
workspace archive digest
`sha256:6a550642ab70cdd6bf9eaa3a71ec379a7e2cfe1588819c17720947c0740400e2`,
and ndn-svs archive digest
`sha256:bc91a91fc4654eda6be3acd1cdb4ed5e62701a2a391a26e6d5535767e1ca8201`.

`build-foundation-local.sh` completed successfully against that exact committed
tree. The complete log is retained at
`results/spec110-itiger-qwen-live/foundation-build-edeeff1/build.log`, and the
machine-readable result is
`results/spec110-itiger-qwen-live/foundation-build-edeeff1/local-foundation.json`.
The build evidence includes:

- ndn-cxx 181 actions, NFD 147 actions, and NDNSF 70 actions completed;
- OpenABE's upstream suites passed, including 46, 36, 106, 1, and 11-test
  groups plus the CLI encryption/decryption regressions;
- the exact sealed NAC-ABE source ran 28 cases and ended with
  `*** No errors detected`;
- the Builder probe reported NFD/NFDC `24.07`, NDNSF pkg-config `0.1.0`, the
  exact source revision, and no unresolved NDNSF shared-library dependency.

The accepted local Foundation image is
`ghcr.io/matianxing1992/ndnsf-di-foundation:spec110-foundation-edeeff1e3041e941b48ba18784348fc9505d7418`
with local image ID
`sha256:94f1c5722e0eb1393c31af76cddecedc5cf30fdc636eac842e506ba14d4cdee6`
and size 360566424 bytes. It is intentionally a `FROM scratch` transfer layer:
it contains only `/opt/ndnsf-di`, is labeled `contains-gpu-runtime=false`, and
is not a standalone shell-bearing runtime. Executable probes therefore run in
the same-source `foundation-builder`; the reviewed GitHub GPU-delta stage must
copy this Foundation into its CUDA runtime before runtime/GPU probes.

The source-bound tag was pushed exactly once. GHCR returned immutable manifest
digest
`sha256:801e8cabc5e084347cb835f107bd9e4c36f07543827c078f8b4720cbf1b48df8`;
the authenticated manifest binds config digest
`sha256:94f1c5722e0eb1393c31af76cddecedc5cf30fdc636eac842e506ba14d4cdee6`
and one 92973858-byte compressed layer. The push log is retained at
`results/spec110-itiger-qwen-live/foundation-build-edeeff1/push.log`.

The first anonymous digest inspection was then executed exactly once and
returned `unauthorized`, because the newly created GHCR package retained its
default private visibility. This is a visibility gate, not a build or push
failure, and another push cannot change it. After the operator changed that
package to Public in GitHub Package settings, a new anonymous inspection of the
same immutable digest passed. Its manifest is retained at
`results/spec110-itiger-qwen-live/foundation-build-edeeff1/anonymous-manifest.json`
and binds the same config digest and one compressed layer. No rebuild, reseal,
or repeat push occurred.

## Audit verdict

**PASS for T166 and the reviewed build graph, local Foundation candidate,
immutable publication, and anonymous-pull boundary.** No unresolved BLOCK/HIGH
issue remains in the static/local route after the OpenSSL ABI,
NAC-ABE test-launch, Python/Focal closure, NCCL base-image, GHCR anonymous-pull,
runtime-library, and sealed-source gates above. The complete committed/sealed
`Dockerfile.foundation` build, same-source Builder probe, one source-bound push,
immutable digest record, and anonymous access have passed. This is a Foundation
release claim only; it is not a CUDA, ONNX Runtime GPU, SIF, or Qwen inference
claim. T167 remains a separate manually reviewed, exactly-once dispatch.

## Replacement Foundation candidate (T169)

The reviewed T168 repair was committed and pushed as
`8f6332ce800a1a5130f457fa54454ad968dff638`. Its verified source seal is
`sha256:a8a846bbd6b40ae5e138ecc72eaac39ebd484e52cfe6b92ffa532a30400cc2c3`;
the seal binds workspace archive
`sha256:c7c0eca51d3a27b84cd49f60cc219348e06b7d21d6cabab7dd2bf6312d409171`
and the unchanged lock
`sha256:0b617bb8a463734c837422786178e455abc327150d6cdbd0eccff16b6460312a`.

`build-foundation-local.sh` rebuilt the Foundation from that exact commit and
seal. ndn-cxx completed 181 actions, NFD completed 147 actions, OpenABE's
upstream suites and CLI positive/negative probes passed, the exact NAC-ABE
source completed all 28 cases with no errors, and NDNSF completed all 70 build
actions. The same-source Builder reported NFD/NFDC `24.07`; the resulting
scratch Foundation has local image ID
`sha256:9596a6d9405730ac02c76a64c5398212f91040baf8ddba244d6d5d85f0751dca`,
size 360566424 bytes, the exact source-revision label, executable NFD/NFDC, 16
measured runtime-system-package entries, and the NDNSF shared library. The
complete build evidence is under
`results/spec110-itiger-qwen-live/foundation-build-8f6332c/`.

The source-bound tag
`ghcr.io/matianxing1992/ndnsf-di-foundation:spec110-foundation-8f6332ce800a1a5130f457fa54454ad968dff638`
was proved absent and pushed exactly once. GHCR returned immutable digest
`sha256:a9ed75a9fa09acd6e795007e5d58a69fb9f9b349222faab9aeb852c70fbed820`.
An anonymous digest inspection passed and bound config
`sha256:9596a6d9405730ac02c76a64c5398212f91040baf8ddba244d6d5d85f0751dca`
plus one 92973852-byte compressed layer. The push log and anonymous manifest
are preserved in the same evidence directory. No second push occurred.

**T169 verdict: PASS.** This accepts only the replacement Foundation and its
anonymous immutable digest. CUDA, ONNX Runtime GPU, final OCI, SIF, Slurm, and
Qwen claims remain gated by T170, T171, and the later live tasks.

## Qwen-minimal replacement Foundation candidate (T173)

The audited T172 repair was committed and pushed as
`94fc25062aa7fced301b1f9db983de3e9a8910e3`. Its independently created and
verified source seal is
`sha256:39cdd27ea73c94ed2ff6801f20f3bef2dbac247e74f55fc3ad67394a87595712`;
the seal binds workspace archive
`sha256:2b7f2947792103c45fdbf3830a93fe7cc0ee5784f37985e0322f6b748549a685`,
the eight locked dependency archives, and lock digest
`sha256:e112ef00cfd0aec3efb4b0fb8140782b1a6973ed4e95ce56fbfed1c1fa3638a5`.

The source-bound tag was proved absent before the build. The exact sealed
Foundation rebuild then completed ndn-cxx 181/181, NFD 147/147, OpenABE's
upstream and positive/negative CLI probes, NAC-ABE 28/28 with no errors, and
NDNSF 70/70. The same-source Builder reported NFD/NFDC `24.07`. The resulting
scratch image has local ID
`sha256:68f63934fd7cf92439ec423b76a39b2fc01e1b41380793bc581e718c6bcdb6fe`,
size 360566424 bytes, the exact source-revision label, 16 measured runtime
system packages, and the NDNSF pkg-config record.

The tag
`ghcr.io/matianxing1992/ndnsf-di-foundation:spec110-foundation-94fc25062aa7fced301b1f9db983de3e9a8910e3`
was pushed exactly once. GHCR returned immutable digest
`sha256:d2aaca7b18aa56b9e24ac6b9c6c9c6a98a26117c32d70966845ae1589ea62d15`.
An anonymous digest inspection passed and bound the same local image ID as its
config plus one layer with digest
`sha256:c9ba3c66c7266eeaebc64d0b42c4ef0017589a182b54347a2384dccfabe98898`
and size 92973874 bytes.

At NDNSF action 67/70 the local filesystem reached capacity and `tee` could no
longer extend `build-and-push.log`. The build process itself continued to the
successful gates and single push, and `local-foundation.json`, the local image
inspection, copied scratch contents, and the anonymous remote manifest all
independently verify the accepted result. This logging failure is retained
rather than hidden. After verification, only disposable Builder images and
24.02 GB of fully reclaimable BuildKit cache were removed; the final images,
seal, and evidence remain. Evidence is under
`results/spec110-itiger-qwen-live/foundation-build-94fc250/`, with the sealed
archives and manifest retained in its `source-seal/` subdirectory.

**T173 verdict: PASS.** The Foundation boundary is accepted. CUDA, ONNX
Runtime GPU, final OCI, SIF, Slurm, and Qwen inference remain gated by a newly
authorized T174 dispatch and terminal T175 evidence; no GPU workflow was
dispatched during T173.

## Vendored-ELF-closure replacement Foundation candidate (T180)

The audited T179 repair was committed and pushed as
`01f730d122a4408737443d75a65dc0d5ff99af5b`. Its independently created and
verified source seal is
`sha256:dd55acbc8fc5af5a3f2b2a8b0eda7c9457d13356e35350629dba47bd870be298`;
the seal binds workspace archive
`sha256:e409698a1c9c684523054c49898ac31ed78ee24901f73b70619630f70ed20582`,
eight locked dependency archives, and lock digest
`sha256:e112ef00cfd0aec3efb4b0fb8140782b1a6973ed4e95ce56fbfed1c1fa3638a5`.
Creation and independent verification returned the same seal digest.

The source-bound tag was proved absent before the build. The exact sealed
Foundation rebuild then completed ndn-cxx 181/181, ndn-svs 10/10, NDNSD 5/5,
NFD 147/147, OpenABE's upstream suites and CLI positive/negative probes,
NAC-ABE 28/28 with no errors, and NDNSF 70/70. The repaired fail-closed runtime
closure derivation also completed inside the real build. The same-source
Builder reported NFD/NFDC `24.07` and NDNSF `0.1.0`.

The resulting scratch image has local ID
`sha256:244427577b722d5f18d1f7c39bf1ee8584dc6bd7942804a670ba00319fb763b0`,
uncompressed size 360566424 bytes, the exact source-revision label, 16 measured
runtime-system-package entries, and the NDNSF shared library. Because this is
an intentional scratch image, it contains no shell; filesystem inspection used
`docker create` plus `docker cp`, while executable version probes ran against
the same-source Builder before that disposable image was removed.

The tag
`ghcr.io/matianxing1992/ndnsf-di-foundation:spec110-foundation-01f730d122a4408737443d75a65dc0d5ff99af5b`
was pushed exactly once. GHCR returned immutable digest
`sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa`.
An inspection with a fresh empty Docker configuration proved anonymous digest
access and bound config
`sha256:244427577b722d5f18d1f7c39bf1ee8584dc6bd7942804a670ba00319fb763b0`
plus one compressed layer with digest
`sha256:eccbd670b7f79d9cbfe4fc8d2288a74d314151e3718f4e324fa417fb5854684e`
and size 92973900 bytes. The anonymous manifest hash equals the published
manifest digest. Build log, local result, anonymous manifest, and complete
source seal are retained under
`results/spec110-itiger-qwen-live/foundation-build-01f730d/`. After
verification, only the disposable Builder tag and 5.916 GB of fully
reclaimable BuildKit cache were removed; the final image and evidence remain.

**T180 verdict: PASS.** This accepts only the repaired Foundation boundary.
Run `29309152207`, its source/Foundation/release identities, and every earlier
failed candidate remain frozen. CUDA, ONNX Runtime GPU, final OCI, SIF, Slurm,
and Qwen inference are still unproven. A new GPU assembly requires a fresh,
candidate-specific explicit human authorization and a new exactly-once source
and release identity; T180 itself dispatched no workflow and submitted no
iTiger job.
