# SIF + APP local-first verification record

本记录对应本地工作区的 SIF + 外置 APP 交付工具。它只记录已经完成的静态、离线和输入门禁检查；真实 SIF、Apptainer 应用启动、C++ 请求链和 TigerCluster 运行必须在获得有效制品后另行记录。

## Review identity

- review snapshot: `.codex-tmp/sif-app-review-20260915-r27`
- `changes.diff` SHA-256: `sha256:c870ebd8839ff6963949ad6277d9a4e60e180515fdc8e7f43b38895e03093cf6`
- snapshot files: 8; all `files.sha256` entries verified
- read-only reviewer result: `STATIC_PASS`
- review lanes: static/security, compile-link/toolchain, runtime/lifecycle, packaging/ABI, tests/evidence/docs
- review scope: exact frozen snapshot; no build, test, edit or commit by reviewer

## Local checks

The following checks passed on 2026-09-15:

```text
pytest -q Experiments/TigerCluster/tests  -> 78 passed
python3 -m py_compile build-sif-app.py validate-sif-app.py test_sif_app.py -> PASS
bash -n build-sif-app.sh run-sif-app.sh -> PASS
shellcheck -x build-sif-app.sh run-sif-app.sh -> PASS
git diff --check -- Experiments/TigerCluster -> PASS
```

The focused C++-free regression checks cover immutable staging cleanup,
atomic intent tokens, placeholder recovery, marker FIFO rejection without
blocking, manifest symlink/inode protection, orphan marker-temp cleanup, and
source inode checks before publication. The runner now has a no-follow scratch
cleanup trap and the pair manifest binds the required stable base paths.

The local input gates behave fail-closed:

```text
build-sif-app.py with missing base/candidate -> status 4 APP_SIF_MISSING
run-sif-app.sh --local with missing base -> status 4 APPTAINER_PAIR_BASE_SIF_MISSING
```

The installed launcher reports Apptainer `1.5.3`; its resolved executable
`/usr/bin/apptainer` has SHA-256
`sha256:2cbfdcbc53a0a1eb56a1327cc42c4cfbdb48beaa03b9a26547df9e4556d3b673`.

## r29 hardening review

- review snapshot: `.codex-tmp/sif-app-review-20260915-r29`
- `changes.diff` SHA-256: `sha256:b9605692e2b661f98575bf569af945aacaad4598d0456b7585fcbe860b073d6a`
- six snapshot files matched their recorded hashes
- read-only reviewer result: `STATIC_PASS`
- the review confirmed FD-pinned base/candidate Apptainer probes, fixed
  scratch parent/root descriptors with inode rechecks, FD-pinned scratch and
  home mounts, and the `ndnsf-sif-app-v2` contract
- compile/link, real Apptainer execution, C++ request-chain behavior,
  abnormal-signal cleanup and TigerCluster qualification remain unobserved

## Dynamic boundary

No regular `.sif` file is present under `Experiments/TigerCluster/`. The only
local image entry is the broken compatibility symlink
`images/spec180-runtime-r119.sif -> ../../../.local-tmp/spec180-candidate-r119/spec180-runtime.sif`.
Therefore this checkpoint does not claim pair materialization, local C++
startup, Request/ACK/Selection/Response, error or cancellation behavior,
ABI loading, or Slurm/Tiger qualification. Upload and final Tiger validation
remain pending until a valid base SIF and container-built candidate SIF plus
its build record are supplied.

## r35 pair-safe runtime review

- review snapshot: `.codex-tmp/sif-app-review-20260915-r35`
- `changes.diff` SHA-256: `sha256:c8be4a42f58cb95baa43f0edd2b91a2c6eb4e27b3e507caacbf07bfb03c4ec18`
- thirteen snapshot files matched their recorded hashes
- official read-only `review-agent` result: `STATIC_PASS`; no P0-P3 findings
- the review covered APP publication callers, Waf/Python runtime paths, complete
  APP copy closure, boundary tests and migration evidence
- pair-safe C++ RPATH is `$ORIGIN/../lib:/opt/ndn-base/lib`; Python bindings use
  `$ORIGIN/../../lib:/opt/ndn-base/lib`; the packager rejects `current`, `stage`
  and `/src/` in ELF RPATH/RUNPATH
- the APP native allowlist now carries the complete candidate ABI closure:
  `libndn-service-framework.so.0.1.0`, `libndnsf-distributed-inference.so`,
  `libndn-svs.so.0.1.0`, `libnac-abe.so`, `libndnsd.so.0.1.0`,
  `libopenabe.so`, `librelic.so` and `librelic_ec.so`; the former DI-only
  layout would leave the pair runner unable to resolve custom NEEDED entries
- candidate and materialized APP checks now require each allowlisted `DT_NEEDED`
  name to resolve from the mounted APP library directory and reject
  `/usr/local/lib`, `current`, `stage` and `/src` resolutions

## Batch retrospective

| Lane | Result |
| --- | --- |
| `static` | r32/r33/r34 findings fixed; r35 official review `STATIC_PASS` |
| `compile/link` | definition, Waf, Python and shell syntax checks passed; no SIF compile/link executed |
| `runtime/test` | 79 Tiger APP/tooling tests passed; no Apptainer runtime or C++ request chain executed |
| `unobserved` | valid base/candidate SIF, builder `%post`, APP materialization, ELF resolution in the pair, C++ Request/ACK/Selection/Response, Slurm and Tiger remain unobserved |

The local build gate remains blocked at input discovery: the recorded base SIF
is not present locally (the `images/spec180-runtime-r119.sif` entry is a broken
compatibility symlink), so no SIF or APP was produced and nothing was uploaded.

## r43 native ABI closure repair

- frozen review snapshot: `.codex-tmp/spec185-sif-app-closure-review-r43`
- `changes.diff` SHA-256: `sha256:b3072b98ab0ffeaf5f0d7c8707fdad5077d9f5d493b16410a3ca5f765a99b432`
- scope: twelve handoff, template, packager, validator, boundary, test, contract and delivery-document files; file hashes are recorded in `files.sha256`
- the APP allowlist now carries the eight candidate libraries required by the
  observed C++ closure: `libndn-service-framework.so.0.1.0`,
  `libndnsf-distributed-inference.so`, `libndn-svs.so.0.1.0`, `libnac-abe.so`,
  `libndnsd.so.0.1.0`, `libopenabe.so`, `librelic.so` and `librelic_ec.so`
- `pytest -q Experiments/TigerCluster/tests` passed `81`; Python compilation,
  Bash syntax, ShellCheck and diff checks also passed
- the handoff renderer now rejects symlinked base/bundle components before
  path resolution; the regression suite records this as an input-gate check
- this is a static/packaging repair only. A real candidate SIF, APP
  materialization, loader startup, C++ request chain and Tiger run remain
  unobserved until a regular base SIF with the locked digest is available

## Local build attempt r1

On 2026-09-15, the real pair materialization command was run with the recorded
Apptainer `1.5.3` executable and the compatibility base path. It stopped before
Apptainer execution at `APP_BASE_SIF_PATH_SYMLINK` with `rc=4`, because the base
path is a broken symlink. The raw command, output and return code are preserved
in `.codex-tmp/local-first-sif-attempt-20260915-r1/`; no candidate, APP or
upload was produced.

## Remote base lookup attempt r1

The recorded cluster path was checked without uploading or modifying remote
state. The SSH alias `tigercluster` could not be resolved by this host
(`rc=255`, `Could not resolve hostname`); the command and output are preserved
in `.codex-tmp/local-first-base-probe-20260915-r1/`. No remote SIF was read and
the local build remains blocked on a regular base SIF and its digest check.

## r46 native ABI closure and local-first gate repair

- frozen review snapshot: `.codex-tmp/spec185-sif-app-closure-review-r46`
- snapshot identity: `base=73c67a47a09d304c47b3486897dd5271e9be2b9f`,
  `changesSha256=sha256:be6a21fa1d6c4b2332869dc8476f76192997bd2e371b6ccf89a3e293e4af1241`,
  `pathsSha256=sha256:31c9cb57a5a40a4f9c33ad18aa428a2c512b4743df7007f3475719d35dbc0534`
- scope: thirteen handoff, template, packager, validator, boundary, test,
  contract, task and delivery-document files; exact file hashes are recorded
  in `files.sha256`
- boundary classification is corrected: native linked-library origin markers
  are checked in the builder stage where `verify-native.py` is created; APP
  materialization origin markers remain runtime checks in the packager
- `pytest -q Experiments/TigerCluster/tests` passed `81`; Python compilation,
  Bash syntax, ShellCheck and diff checks passed
- official read-only `review-agent` result is pending for this frozen snapshot

The local pair gate was rerun after this repair with the recorded Apptainer
`1.5.3` executable. It stopped at `APP_BASE_SIF_PATH_SYMLINK` with `rc=4`
before Apptainer execution because the recorded base path is still a broken
compatibility symlink. The raw command and result are in
`.codex-tmp/local-first-sif-attempt-20260915-r2/`; no SIF, APP or upload was
produced.

## r48 native ELF oracle repair

- frozen review snapshot: `.codex-tmp/spec185-sif-app-closure-review-r48`
- the native regression now retains the complete `DT_NEEDED` set, rejects
  unclassified dependencies, and checks every APP dependency's `ldd` source
  under `NDNSF_TEST_APP_LIB_ROOT` when a container APP root is supplied
- host-only candidates without that APP root are explicitly skipped for the
  origin assertion; the container packager's mandatory APP-first `ldd` check
  remains the runtime gate and rejects `/usr/local/lib` and other forbidden
  roots
- `pytest -q Experiments/TigerCluster/tests`: `80 passed, 1 skipped`; Python
  compilation and diff checks passed
- official read-only `review-agent` review is pending for this immutable
  snapshot

## r49 native ELF oracle boundary repair

- the test derives its base-side NEEDED allowlist from the published
  `baseNativeLibraries` runtime contract, including Boost, SQLite and GMP
  entries, instead of maintaining a second incomplete list
- when `NDNSF_TEST_APP_LIB_ROOT` is present, `ldd` now receives an explicit
  APP-first `LD_LIBRARY_PATH`; the source assertion therefore exercises the
  selected APP/lib directory rather than merely reading the variable
- local result remains `80 passed, 1 skipped` because this host has no APP
  runtime root; no SIF/Apptainer/Tiger execution was performed
- official read-only `review-agent` review is pending for the r49 snapshot

## r50 native candidate path repair

- the real ELF regression now locates reusable `build-spec185-*` trees from
  the repository root (`ROOT.parents[1]` for `Experiments/TigerCluster`), so
  an available native candidate is no longer silently skipped
- focused execution found the candidate and ran the complete `DT_NEEDED`
  allowlist oracle; only the APP-origin subassertion is skipped without a
  container APP root (`APP/lib origin requires a container runtime root`)
- full local Tiger APP tests remain `80 passed, 1 skipped`; no SIF,
  Apptainer runtime or Tiger execution was performed
- official read-only `review-agent` result: `STATIC_PASS`; no P0-P3 findings.
  The immutable snapshot identity is `changesSha256=sha256:2dde13ff9d162d33e76234c10c921ad12dcf019653fd981f42deac8602127d8`,
  `pathsSha256=sha256:1ea9212b7740ff8a9ca4498ccb1fb6117a67cc1484a0f7f0b54f3e115069a2d8`,
  base `73c67a47a09d304c47b3486897dd5271e9be2b9f`
- five lanes are statically closed; actual SIF/Apptainer, C++/Python startup,
  APP-origin inside a materialized pair, Slurm and Tiger remain unobserved

## r51 final static closure

- final immutable review snapshot: `.codex-tmp/spec185-sif-app-closure-review-r51`
- review scope: eleven implementation, contract and test files; progress and
  failure records are maintained separately from the frozen review scope
- snapshot identity: `base=73c67a47a09d304c47b3486897dd5271e9be2b9f`,
  `changesSha256=sha256:02610b4a12aa83af17cc37a491c5c50765761e20eb12f4de21536214572d6f88`,
  `pathsSha256=sha256:75e255e77f49765f79964a95d0ca087ff5da31e5965d7b086770f67639ad60dc`
- official read-only `review-agent`: `STATIC_PASS`, no P0-P3 findings; APP
  eight-library closure, base contract, complete NEEDED oracle, APP-first
  `ldd`, builder/final boundary and forbidden-origin checks are statically
  consistent
- local tests: `80 passed, 1 skipped`; the single skip is only the APP-origin
  subassertion because no materialized APP/lib root exists on this host
- compile/link, SIF/Apptainer runtime, C++/Python startup, Slurm and Tiger
  qualification remain unobserved and are not claimed as PASS
