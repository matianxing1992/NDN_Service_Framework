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

## Local build attempt r1

On 2026-09-15, the real pair materialization command was run with the recorded
Apptainer `1.5.3` executable and the compatibility base path. It stopped before
Apptainer execution stopped at `APP_BASE_SIF_PATH_SYMLINK` with `rc=4`, because the base path is
a broken symlink. The raw command, output and return code are preserved in
`.codex-tmp/local-first-sif-attempt-20260915-r1/`; no candidate, APP or upload
was produced.
