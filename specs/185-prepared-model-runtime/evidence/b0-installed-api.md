# B0 / T015 Installed C++ API and ABI Closure

## Scope and decision

本记录是 B0/T015 的唯一批次证据，覆盖 C-08 CD10/FN09/PO09、C-05 U10/U11/U12 与
C-06 installed-only gate。所有 native assertion、fixture、consumer 和 oracle 均为 C++；
Python、SIF、TigerCluster 未参与本批次。`T015` 在本记录所列验收全部通过后关闭。

| Lane | Result | Evidence |
| --- | --- | --- |
| static | PASS | 官方 `review-agent` v17，snapshot `spec185-t015-review-228aa944-v17`，manifest SHA-256 `006ce2ab1b889d6a5c820a85e15ebf2a6a01f6a871bba3d1ca29e3776991bde6` |
| compile-link | PASS | 99/99 targeted Waf targets in each normal/ASan candidate; external consumer links with system-first `/usr/bin` toolchain |
| runtime-test | PASS | Four external C++ consumers and four in-tree consumers return `rc=0` |
| packaging/ABI/installed-boundary | PASS | 67 standalone installed headers, excluded-header negative gate, pkg-config prefix check, readelf/ldd and DI/SVS hash binding |
| unobserved | NONE for B0 exit | Full Spec185 runtime, Core extraction, Python binding and later qualification remain in their dependent batches |

## Source and review identity

- Base commit: `228aa94450a1353fdd65e1c1900b21a875462ec0`.
- Review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Immutable review snapshot: `.codex-tmp/spec185-t015-review-228aa944-v17/`.
  It contains all 15 changed paths, including the two pkg-config templates and the
  installed-consumer script; no unrelated dirty file was included. v17 returned
  `STATIC_PASS` with no P0/P1/P2/P3 findings.
- The installed DI exposure manifest enumerates 67 public headers: three stable
  umbrellas plus the reviewed compatibility/transitive closure. `NativeRequestEnvelope.hpp`
  and the other eleven internal/worker headers are excluded and remain absent from the
  staged SDK.

## Build configurations and candidates

All commands used `/usr/bin/g++ -B/usr/bin`, `/usr/bin/ld` (binutils 2.34), C++17,
and system-first `PATH=/usr/bin:/bin:/usr/sbin:/sbin`. Configure used the explicit
NAC-ABE install `/home/tianxing/NDN/nac-abe-integration-182/install-spec184-r4`,
ONNX full-protobuf prefix `.codex-tmp/spec182-t001-dependencies/onnx-install`,
and NDN-SVS source/build pair `/home/tianxing/NDN/ndn-svs` plus `/home/tianxing/NDN/ndn-svs/build`.
The selected NDN-SVS library hash is
`12fea93c9b07fee000abb412746e4d170955ec270a51a52579d3851f47c8f1c8`.

| Candidate | Configure identity | Target build result | DI library SHA-256 |
| --- | --- | --- | --- |
| disabled normal | `build-spec185-b0-disabled-normal-v2`, `HAVE_ONNXRUNTIME_CPP=False` | 99/99, 10.523 s | `032f69a9e4801d9726a0609006b1d01810d80e06ea0ef3884e2cd6447ca68c50` |
| enabled normal | `build-spec184-b6-candidate-tests`, `HAVE_ONNXRUNTIME_CPP=True` | 99/99, 5.391 s | `92f2a5cf937c3df3112c73f84fb54610386c3bf0536e5532ef496a298a2d9d36` |
| disabled ASan | `build-spec185-b0-disabled-asan`, `HAVE_ONNXRUNTIME_CPP=False`, `-fsanitize=address` | 99/99, 20m14.921 s, `-j2` after resource policy | `7b6b76989dc952ab62c58ece41149476f2ad324d14ee2d2edf28016a4e712bd3` |
| enabled ASan | `build-spec185-b0-enabled-asan`, `HAVE_ONNXRUNTIME_CPP=True`, `-fsanitize=address` | 99/99, 18m11.387 s, `-j2` after resource policy | `cb9273b1a65425484df763e844a721d871a9418ae60985e0536989fa8a8212cc` |

Target build command for each candidate:

```text
./waf --out=<candidate> build --targets=ndnsf-distributed-inference,spec185-installed-consumer -j<N>
```

The first broad enabled build and the first enabled-ASan `-j4` attempt were preserved as
non-qualification resource/scope boundaries. They are not counted as failures of the
candidate or as PASS evidence.

## Installed consumer and ABI checks

The final command was:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  tests/installed-api/run-spec185-installed-consumer.sh \
  /tmp/spec185-b0-disabled-normal/usr/local \
  /tmp/spec185-b0-disabled-asan/usr/local \
  /tmp/spec185-b0-enabled-normal/usr/local \
  /tmp/spec185-b0-enabled-asan/usr/local \
  .codex-tmp/spec185-b0-external-consumer-r10-20260912
```

The four prefixes contain the exact DI/Core artifacts from the candidates, the selected
NDN-SVS install (15 headers plus generated `config.hpp`, SONAME library and
`libndn-svs.pc`), and pkg-config metadata. Core metadata declares
`Requires: libndn-svs >= 0.1.0`; no NDN-SVS source/build path is exported. The final
consumer output is in `.codex-tmp/spec185-b0-external-consumer-r10-20260912.log` (SHA-256
`44f469c9eb445e48aa83e2726b2038b6be6ebdd68c291cec6299270391e6c244`).

Each mode reports `standalone_headers_pass=67` and
`excluded_header_negative=PASS`. The negative translation unit includes
`NativeRequestEnvelope.hpp` and must fail because the internal header is not installed.
Every consumer has a DI `NEEDED` entry and ldd resolves both
`libndnsf-distributed-inference.so` and `libndn-svs.so.0.1.0` inside its own prefix.
The script compares `readlink -f` selected/loaded paths and records both hashes in
`pkg-*/library.sha256`; no Python/libpython dependency is present in the consumer ELF.

The in-tree selectors also passed:

- enabled normal: `.codex-tmp/spec185-b0-in-tree-consumer-20260912.log`;
- disabled normal: `.codex-tmp/spec185-b0-disabled-normal-v2-in-tree-consumer-20260912.log`;
- enabled ASan: `.codex-tmp/spec185-b0-enabled-asan-in-tree-consumer-20260912.log`;
- disabled ASan: `.codex-tmp/spec185-b0-disabled-asan-in-tree-consumer-20260912.log`.

The enabled selector writes and loads a 95-byte ONNX Identity graph, constructs the
registered ONNX runner, and destroys it. The disabled selector verifies the explicit
`C++ ONNX Runtime backend is not enabled` construction boundary. No full request
qualification is claimed by this B0 fixture.

The required single-task B0 composition review was then rerun on the same immutable v17
snapshot and returned `B0_COMPOSITION_PASS`. It checked the complete Waf/pkg-config to
67-header/negative to ONNX enabled/disabled factory to ELF `NEEDED` to prefix-bound
DI/SVS `ldd`/hash to normal/ASan consumer flow and found no P0-P3 issue.

## Batch retrospective and closure

Static review caught the disabled `runStreamed` link closure, ONNX disabled include/session
guards, PImpl layout, internal-header dependency leak, actual ldd/hash binding, and the
later package metadata/SVS identity boundaries before the final retry. Compile/link and
runtime checks were then run once per final candidate matrix. Earlier attempts are retained
under `.codex-tmp/spec185-b0-external-consumer-*.log` and documented in `docs/failure-log.md`.

**Closure decision: `CLOSED_FOR_VALIDATION` for B0/T015.** This closes only installed API/ABI
exposure. B0C/T017 remains the next dependency-satisfied batch; no later Spec185 task is
implicitly complete.
