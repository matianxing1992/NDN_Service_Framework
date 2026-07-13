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
- Boost 1.74.0 official source archive: 109600630 bytes, SHA-256
  `83bfc1507731a0906e387fc28b7ef5417d591429e51e788417fe9ff025e116b1`.
  It is a checksum-bound `sourceArchives` member of the source seal, is built
  into the common prefix before ndn-cxx, and replaces Focal's insufficient
  Boost 1.71 without a PPA or distribution change.
- ONNX Runtime GPU C++ 1.20.1 Linux x64 archive: 258487100 bytes,
  SHA-256 `6bfb87c6ebe55367a94509b8ef062239e188dccf8d5caac8d6909b2344893bf0`.
- Docker Registry config blobs for both pinned CUDA build and runtime digests
  report CUDA 12.4.1, NCCL 2.21.5-1, and cuDNN 9.1.0.70-1. NCCL is therefore a
  measured base-image input for distributed inference, not an assumed or
  silently omitted PyPI dependency.

## Static evidence

The revised build-graph unit suite passes 14/14 cases. It verifies local-only sealed source
consumption, dependency order, NFD inputs, OpenABE adapter, ONNX asset identity,
Python closure, explicit installation of `App_ServiceController` and
`di-native-provider`, runtime closure, Qwen exclusions, dispatch-only workflow,
transient-seal Git hygiene, and failure evidence retention. The release pipeline shell integration passes,
the lock parses as JSON, Python scripts compile, shellcheck passes, and Docker
successfully parses/builds the external-foundation stage.

After the Boost correction, the complete Spec 110 offline suite passes 98/98,
the targeted sealed-source/build-contract suite passes 21/21, the release
pipeline integration passes, and the seven changed source/document files scan
with zero secret findings across 71130 bytes.

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

NAC-ABE commit `1cc17d9d21f4dfc0921cc77315d0c57d46291880` discovers system
OpenSSL and `libopenabe` through CMake and has no independent bundled OpenSSL.
Its upstream CMake registers tests only below the `tests/` subdirectory, so a
root-level `ctest` can misleadingly report no tests. The foundation gate instead
builds with `-DHAVE_TESTS=TRUE` and executes the unit-test binary from the
directory containing `trust-schema.conf`; missing test configuration is a hard
failure, not an accepted crypto result.

The corrected local command executed all 29 NAC-ABE cases in 63.44 seconds and
ended with `*** No errors detected`. This includes CP/KP setup, key generation,
encryption/decryption, concurrent OpenABE state serialization, integrated
producer/consumer flows, retries, 1000-operation encryption/decryption
benchmarks, and 1000/1000 successful cached decryptions. An earlier invocation
from the wrong directory produced 18 missing-`trust-schema.conf` failures; it
is retained as a test-launch error and was not counted as cryptographic
evidence.

## First complete-build attempt and Boost correction

Committed candidate `001b48b4f7d2fded1480c0d59c6482a51ea2edbd` was sealed as
`sha256:e8e181eb251ef1fee36c909715263a4d9f4dfa2895aca5255e29e1950594d78d`
and started the first complete Foundation build. The pinned Ubuntu 20.04
system closure installed successfully, ndn-cxx configured against OpenSSL
1.1.1f and compiled all 181 build actions, but ndn-svs then rejected the
distribution Boost 1.71 because its minimum is 1.74. This is a preserved local
`EXECUTED_FAIL`, not a release candidate or a reason to change the OpenSSL ABI
base. The complete log is retained at
`results/spec110-itiger-qwen-live/foundation-build/build.log`.

The correction adds a sealed non-Git source-archive contract, archive/path
safety validation, tamper tests, and explicit Boost include/library paths for
the NDN builds. The targeted regression is 21/21 PASS. Because the lock and
workspace changed, the failed candidate's seal is retired; the next full build
must use a new commit and newly generated seal.

## Audit verdict

**PASS for the reviewed build graph; T166 remains open.** No unresolved
BLOCK/HIGH issue remains in the static/local route after the OpenSSL ABI,
NAC-ABE test-launch, Python/Focal closure, NCCL base-image, GHCR anonymous-pull,
runtime-library, and sealed-source gates above. This is not a foundation release
claim: the complete committed/sealed `Dockerfile.foundation` build, in-container
probe, source-bound push, and immutable digest record still belong to T166.
The first full build was started and preserved as the Boost-version failure
above. Useful Docker build cache remains available for the corrected candidate;
T166 stays open until that new candidate passes every in-container gate and is
published by immutable digest.
