# Design Decisions

## Decision 1: Use official CUDA images, not a full framework image

Use the existing digest-pinned NVIDIA CUDA 12.4.1 cuDNN development/runtime
compatibility line, but use the runtime image for both ML products. The current
source compiles no CUDA kernels; ONNX Runtime supplies its own C++ headers, and
the later NDN builder installs the ordinary C++ toolchain. The CUDA devel image
therefore adds a 2.63 GB compiler layer without a consumer. A preassembled
PyTorch/NGC development image would be still larger and would not preserve the
accepted Python/ORT closure.

## Decision 2: Separate ML, stable NDN, and mutable App ownership

The current foundation incorrectly contains ndn-svs and NDNSF, while the GPU
assembler rebuilds NDNSF. Since ndn-svs and NDNSF change frequently, both move
to the App layer. Code inspection also proves NDNSD links to `libndn-svs`, so
NDNSD must move with ndn-svs rather than remain in the stable layer. Stable
NDN/security dependencies remain below them.

## Decision 3: Keep development and runtime products

The App builder needs headers and tools, while the final image should not carry
compilers or source archives. Each stable boundary therefore exposes matching
development and runtime images.

## Decision 4: Verify lock-derived parent tags against local image IDs

Buildx `FROM` resolution uses lock-derived, write-once local tags because raw
daemon-local image IDs are not a portable parent reference. The driver resolves
and records each ID, then verifies the tag-to-ID binding before and after child
builds. Human/floating tags are never accepted as parents.

## Decision 5: Treat local build as development authority

The host lacks a GPU and the current workspace may be dirty. Local closure and
static health checks can prove build correctness and reuse, but cannot prove
CUDA execution or formal source reproducibility. Those remain explicit later
release gates.

## Alternatives considered

- **Keep current two-file split**: rejected because ndn-svs and NDNSF remain in
  the expensive foundation and NDNSF is compiled again in the GPU stage.
- **One multi-stage Dockerfile only**: rejected because retaining/tagging
  independently reusable products and their locks is less explicit.
- **Install all dependencies in the final App image**: rejected because small
  source changes invalidate the whole build graph.
- **Base the image on NGC PyTorch**: rejected because it broadens the runtime,
  increases image size, and weakens the existing version contract.
