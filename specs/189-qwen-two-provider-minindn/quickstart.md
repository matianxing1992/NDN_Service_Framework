# Spec189 Quickstart

Run from the repository root. Read `docs/failure-log.md`, `docs/architecture-reading-guide.md`, this Spec189 `spec.md`, `plan.md`, `tasks.md`, and the newest evidence before starting.

## 1. Candidate freeze

Use a unique run directory under `.codex-tmp/spec189-qwen-two-provider-<timestamp>/`. Record:

- Experimental source commit and changed-file digest;
- compiler/binutils, system Boost 1.71, NDN-CXX/SVS/NAC-ABE/ORT library hashes;
- Qwen snapshot/revision, tokenizer/config, canonical graph, external initializer and both layer package hashes;
- profile, topology, two provider nodes, memory/disk floors and selector hashes.

The stage manifest must describe exactly two stages and ranges `0..14` and `14..28`. Do not use a historical result or a preloaded runner.

## 2. Prepare and verify Repo publication

Run the maintained native authority/prepare path (the exact command is frozen in `tasks.md` after CodeGraph verifies the current CLI). Its C++ output must include `PREPARE`, `REPO_COMMIT`, `MANIFEST_DIGEST`, `LAYER_REF_0`, `LAYER_REF_1` and `PREPARE_SOURCE_RELEASED` markers. A stage-export-only output is `NOT_READY`.

## 3. Build affected C++ targets

Use the matching existing Waf build tree and the smallest affected closure. Default to `-j4`; reduce to `-j2` only after observing sustained swap or host stalls. Build/verify `DI_NativeArtifactAuthority`, `DI_NativeRequester`, `DI_NativeOnnxAssemblyWorker`, `di-native-provider`, `App_ServiceController` and the new Spec189 C++ oracle target. Record `nm -C`/`readelf` definitions and `ldd` identities.

## 4. Run real MiniNDN

Run as root with `/usr/sbin:/sbin` in PATH, one active subject, a unique run id and a resource sampler. The maintained Python script may launch the C++ authority/requester/providers and stop them, but must not inject a synthetic ACK/Selection or call ORT directly. The event order must be:

```text
PREPARE → REPO_COMMIT → REQUEST_REFERENCE_ONLY → ACK → SELECTION_2_PROVIDERS
→ PROVIDER_0_FETCH/ASSEMBLE/EXECUTE → PROVIDER_1_FETCH/ASSEMBLE/EXECUTE
→ TERMINAL → DRAINED
```

On a memory floor, timeout, protocol mismatch, child leak or digest failure, stop deterministically and write the first boundary. Do not call it PASS.

## 5. Repeat and classify

After the first run is durable, repeat with a new run id and the same immutable candidate tuple. `QWEN_TWO_PROVIDER_PASS` requires both runs to agree on the full event sequence, terminal oracle and cleanup. Otherwise leave the task `PARTIAL`, `RESOURCE_BOUNDARY`, `PROTOCOL_BOUNDARY` or `UNQUALIFIED`.
