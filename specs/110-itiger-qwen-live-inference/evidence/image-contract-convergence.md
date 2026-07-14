# Spec 110 experiment-ready image contract convergence

**State**: implementation in progress; no cloud build or Slurm submission was
authorized by this convergence task.

## Superseded pre-start candidate

The Phase 18 candidate bound to source
`01f730d122a4408737443d75a65dc0d5ff99af5b` and Foundation digest
`sha256:71711601004a6f032fda032037691409324d2adb867efcd94be48b2d879227aa`
is `PRE_START_SUPERSEDED`. No candidate source tag, release tag, GitHub workflow
dispatch, GHCR runtime publication, or iTiger job was created for T181-T183.
The Foundation remains valid historical build evidence; it is not a valid
Foundation input for source changed after this audit because `Dockerfile.gpu`
checks its embedded source revision fail closed.

## Controlling audit findings

1. The official `Dockerfile.gpu` did not install the three Qwen experiment
   entrypoints referenced by the Slurm route.
2. An unused Qwen overlay Dockerfile installed a conflicting second Python
   lock, so it could not safely repair the official image.
3. `ndnsf-qwen.sbatch.in` bypassed `run-container.sh` and exposed the entire
   project root writable inside the allocation.
4. The canonical runner did not provide an explicit read-only artifacts mount.

These findings blocked a new image build. The repair makes the official GPU
Dockerfile the sole final recipe, keeps `gpu.lock` as the sole dependency
contract, installs all experiment entrypoints in `/opt/ndnsf/bin`, and routes
Qwen execution through explicit least-privilege mounts.

## RED evidence

`python3 tests/container/itiger-qwen-live/unit/test_github_sealed_workflow.py`
ran 18 tests and failed exactly two new contract tests before implementation:

- official GPU image packages Qwen entrypoints without an overlay lock;
- Qwen Slurm uses the canonical least-privilege runner.

After implementation the same focused suite passed 18/18. The release pipeline
integration test also reached `RELEASE_PIPELINE_PASS` and observed
`/artifacts:ro` with no writable whole-project bind. Full offline, secret,
sealed-source, and image-build evidence is appended only after those gates run.

## Post-implementation gate

- Spec 110 offline suite: 103/103 PASS, zero failures/errors/skips;
- container integration scripts: 5/5 PASS, including release materialization,
  runtime compatibility, rootless fallback, network scripts, and packaged
  security contract;
- official build-context secret scan: 107 files, 373,011 bytes, zero findings;
- GPU build preflight: PASS;
- workflow YAML parse: PASS;
- changed container/Slurm shell syntax: PASS;
- CodeGraph synchronization: already up to date;
- strict Spec Kit structural audit: PASS, 37/37 requirements traced and 190
  tasks parsed;
- `git diff --check`: PASS.

An intentionally broader full-repository secret scan was also run and failed
with 42 findings in historical experiment keys, hydrated `RELEASE/` payloads,
token implementation syntax, and scanner negative fixtures. This is not hidden
or counted as a build-context pass. Those paths are outside the official
workflow scan root and are excluded by the OCI `.dockerignore`; the immutable
seal/build-context scan remains the controlling publication gate. Similarly, a
naive `bash -n` over every tracked `*.sh` found the existing
`Experiments/start_ucla.sh`, whose content is MiniNDN Python despite its suffix.
The four changed container/Slurm scripts and templates passed `bash -n`.

## First complete local GPU build outcome

The exact `de849b88c25337cc9acf55f6572b081b2f00ab9e` worktree completed
all CUDA base pulls, Python/CUDA package installation, ONNX Runtime SDK
verification, NDNSF-DI C++/ONNX compilation, and all three local Python wheels.
After 31 minutes 4.50 seconds it failed closed during the final ELF scan:

```text
RUNTIME_LIBRARY_MISSING:/opt/venv/lib/python3.10/site-packages/torch/lib/libcaffe2_nvrtc.so
```

Independent `readelf -d` inspection of the exact PyTorch 2.6.0+cu124 wheel
showed `libcaffe2_nvrtc.so` needs `libnvrtc.so.12` and `libcuda.so.1`.
The former is supplied by the locked CUDA/Python userspace closure. The latter
is the NVIDIA driver ABI and must be injected by the iTiger host through
`apptainer exec --nv`; embedding a host driver in the image would violate
FR-003. This attempt is retained as `EXECUTED_FAIL`, and no image tag was
created.

The host-driver regression was RED against the original scanner and failed only
because `libcuda.so.1` was unresolved. The repaired scanner parses unresolved
SONAMEs and permits exactly `libcuda.so.1`; the missing sibling DSO negative
test remains fail closed. Focused closure tests passed 3/3, sealed-workflow
tests passed 18/18, the complete Spec 110 offline suite passed 104/104, the
official build-context secret scan remained clean, and the strict structural
audit passed with 37/37 requirements traced and 194 tasks parsed.

## Second complete local GPU build outcome

Source `228341e0ea1f28956015fbaa30d2bf58a56b7789` and local Foundation
`sha256:a07c4470ed89b1d7da8fd626f94c4dc9062a2125dc969242a4588d5d87cf7158`
completed the same build stages and passed the `libcuda.so.1` boundary. After
20 minutes 10.79 seconds the next fail-closed scan found:

```text
RUNTIME_LIBRARY_MISSING:/opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_tensorrt.so
```

The accepted backend is `CUDAExecutionProvider`, not TensorRT. The
`onnxruntime-gpu` wheel includes an optional TensorRT provider DSO whose
TensorRT userspace libraries are intentionally absent. Treating those
libraries as host-driver injections would be unsafe. The replacement must
lock and remove that optional provider before closure scanning. This attempt is
`EXECUTED_FAIL`; no final image tag or external publication exists.

The replacement contract now records
`onnxRuntimeExcludedOptionalProviders=["TensorrtExecutionProvider"]`, removes
only `libonnxruntime_providers_tensorrt.so`, and leaves every CUDA provider and
closure gate intact. `FOUNDATION_SOURCE_REVISION` is validated independently
from the final `SOURCE_REVISION` and is retained as an OCI label and release
manifest field alongside the immutable Foundation image digest. The focused
workflow suite passed 20/20, the complete Spec 110 offline suite passed
106/106, all five shell integrations passed, the official source scan reported
zero findings, YAML/preflight/diff checks passed, and the strict Spec Kit
structure audit passed with 37/37 requirements traced and 198 tasks parsed.
The post-implementation audit verdict for this bounded repair is `PASS`; the
overall Spec 110 live-experiment feature remains incomplete until the final OCI
image and later iTiger execution evidence exist.

## Third complete local GPU build outcome

Source `9ea1d9a252b9a60e535ca77ee5f073b59f59de72`, seal
`sha256:a13f86c044449c7b8ea80e3c7d09e0d2e7a87c4d4a4aa7dca654dab82a796e57`,
and immutable Foundation source `228341e0ea1f28956015fbaa30d2bf58a56b7789`
passed the independent component binding, Python package closure, removal of
the Python-wheel TensorRT provider, all 244 C++ targets, ONNX Runtime C++
backend linkage, `di-native-provider` linkage, and all three Python wheel
builds. After 20 minutes 29 seconds the fail-closed scan found the second copy:

```text
RUNTIME_LIBRARY_MISSING:/opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so
```

The independently downloaded ONNX Runtime C++ GPU SDK ships the same unused
optional provider alongside the required CUDA provider. This is not a new
dependency requirement and must not be allowed as a host injection. The next
candidate must lock and remove both the SDK and Python-wheel TensorRT provider
DSOs before scanning while retaining the CUDA provider DSOs. This attempt is
`EXECUTED_FAIL`; 246 disk samples recorded a peak root usage of 144582164480
bytes and a minimum available capacity of 35949211648 bytes. No final image,
GHCR publication, or iTiger job exists for this identity.

The replacement now asserts both required CUDA provider DSOs exist, removes
both TensorRT provider DSOs in one lock-gated step, and then asserts both are
absent before package and runtime closure derivation. The focused workflow
suite passed 20/20 and the complete Spec 110 offline suite passed 106/106.
Preflight, workflow YAML, diff, source secret scan with zero findings, and the
strict structural audit all passed; the latter traced 37/37 requirements and
parsed 202 tasks. The bounded post-implementation audit verdict remains
`PASS`, while final OCI and iTiger execution claims remain unproven.

## Fourth complete local GPU build outcome

Source `40cd15f30886b065aa204be77d5d0784a48edc3f`, seal
`sha256:8c985b14cc44807016520f9114ba752d2fc9edb4583fddc69d28988e37c3ba36`,
and immutable Foundation source `228341e0ea1f28956015fbaa30d2bf58a56b7789`
passed both required CUDA-provider assertions, removed both optional TensorRT
provider DSOs, completed all 244 C++ targets, linked the native ONNX provider,
built all three Python wheels, and derived the final runtime package closure.
After 21 minutes 15 seconds the final CUDA runtime layer rejected this APT
operation:

```text
E: Held packages were changed and -y was used without --allow-change-held-packages.
```

The derived closure included `libcudnn9-cuda-12`, which was already installed
and intentionally held in the digest-pinned NVIDIA CUDA runtime base. A newer
repository candidate made a plain `apt-get install` attempt to upgrade that
base-owned package. Allowing held-package changes would silently mutate the
locked CUDA/cuDNN stack, so it is not an acceptable repair. The replacement
filters the derived manifest through `dpkg-query` and installs only packages
whose current-state abbreviation does not report installed.

This attempt is retained as `EXECUTED_FAIL`; 254 disk samples recorded a peak
root usage of 151829037056 bytes and a minimum available capacity of
28702339072 bytes. No final image, GHCR publication, or iTiger job exists for
this identity. The missing-package-only policy has a focused RED/GREEN
regression and is enforced by the static GPU-build preflight.

## Fifth complete local GPU build outcome

Source `a36d8de06ac4c392a4f75a2fb891937a4b06f57d`, seal
`sha256:ab5f8b7264ba69743008066d7d4a32b3ed4c5f6577da1d1f830d666a291c50dc`,
and the same immutable Foundation source completed the CUDA/Python/ONNX
assembly, both TensorRT removals, 244/244 C++ targets in 12 minutes 41 seconds,
native ONNX linkage, all three Python wheels, and final runtime package
derivation. After 21 minutes 7 seconds the runtime install again rejected a
held-package upgrade.

The filter itself executed, but `dpkg-query -W -f='${Status}'` reports the
digest-pinned cuDNN package as `hold ok installed`. The initial regression
incorrectly required the desired action to be `install`, so it classified the
already-installed held package as missing. A direct probe of the exact CUDA
runtime base confirmed:

```text
libcublas-12-4      hold ok installed  12.4.5.8-1
libcudnn9-cuda-12  hold ok installed  9.1.0.70-1
```

The replacement uses `${db:Status-Abbrev}` and tests the current-state column:
both ordinary `ii ` and held `hi ` packages match `.i `, while a nonexistent
package remains missing. This preserves the pinned CUDA stack without allowing
held-package upgrades.

The failed attempt recorded 253 disk samples, peak root usage of
158200406016 bytes, and minimum available capacity of 22330970112 bytes. No
final image, GHCR publication, or iTiger job exists for this source identity.

## Sixth complete local GPU build outcome

Source `43648aada42fa051ebc09b86727055631aa5f9fa`, seal
`sha256:1eb82a049de6a0cd92f0a911672061570c1d8fdb932eed5d884559e423262b32`,
and the immutable Foundation source completed CUDA/Python/ONNX assembly,
244/244 C++ targets in 12 minutes 34 seconds, native and fault-provider links,
all Python wheels, and runtime package derivation. The held-package repair was
validated by APT itself:

```text
0 upgraded, 24 newly installed, 0 to remove and 34 not upgraded.
```

The run then copied the complete NDN, ONNX Runtime GPU, Python, and Qwen
runtime into the final stage. After 21 minutes 35 seconds the independent
`verify-runtime-closure.py` rejected Pillow and NumPy wheel-private DSOs plus
the PyTorch driver dependency. The package derivation had already handled
these correctly; the final verifier had not inherited its per-ELF sibling
directory lookup or its exact `libcuda.so.1` host-driver exception.

The replacement verifier now applies a temporary `LD_LIBRARY_PATH` containing
only the ELF's own parent plus the inherited image paths, rejects every
unresolved userspace SONAME, and permits only `libcuda.so.1` for later
`apptainer exec --nv` injection. Real ELF tests prove a sibling bundle passes,
deleting its sibling DSO fails closed, and a synthetic missing `libcuda.so.1`
passes. Preflight locks all three policy markers.

The failed attempt recorded 259 disk samples, peak root usage of
162382110720 bytes, and minimum available capacity of 18149265408 bytes. No
final image, GHCR publication, or iTiger job exists for this source identity.
