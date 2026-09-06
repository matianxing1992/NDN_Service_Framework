# Spec 170 local storage cleanup (historical snapshot, 2026-08-16)

> **Historical snapshot; not a current filesystem or release inventory.**
> Statements below such as “no local SIF” describe the state before the
> 2026-08-17 local-SIF-first route and must not be used to plan a new run.
> The current candidate and its hash are recorded in
> `local-sif-build-route-20260817.md`.

This is a closed storage snapshot from before the local-SIF-first workflow.
The paths and remote candidate below are retained only as cleanup provenance;
they are not the current Spec170 release inputs. Use the 2026-08-17 local GPU
SIF evidence and its hash record for the active candidate.

The local release SIF was not the source of the apparent multi-gigabyte
growth.  The current candidate SIF is remote and content-addressed:

```text
/project/tma1/ndnsf-di/releases/spec170-runtime-e7ed-ort1200-svs1d432-20260816/runtime.sif
bytes=4544180224
sha256=505518e21288eb17b0c4a98c86aa34a4f5e051984d5d3a6942e066b375c9c848
```

The following user-owned, reproducible material was removed or emptied:

- six unregistered `.codex-tmp/spec168-v89` through `spec168-v94` worktrees;
- the user cache directories for VS Code C++, Go, uv, pip, matplotlib,
  Mesa shaders, and thumbnails.

The old Gate-B run contained repeated root-owned model payloads and the old
content-addressed cache.  After an explicit no-open-file check (`lsof +D`),
the following exact stale paths were removed with passwordless `sudo`; no
logs, manifests, or tracked evidence in their parent run directories were
removed:

```text
results/spec170-gate-b-minindn-20260805T103000Z/block-{2,3,4,5}/repo-store-0
results/spec170-gate-b-minindn-20260805T103000Z/block-{4,5}/provider-model-cache-{0,1,2}
results/.ndnsf-di-content-addressed
```

The deletion reclaimed `11,545,866,240` bytes.  The parent Gate-B run remains
for its failure evidence, while a future diagnostic can regenerate the
content-addressed payloads from the sealed release.

The Gate-B logs, manifests, and tracked evidence remain.  The stage-bundle
metadata remains while its three reproducible `.pt` payloads were removed;
they can be regenerated from the sealed release if a future diagnostic needs
them.  Before the stale-payload cleanup, root filesystem status after the
user-owned cleanup was `48,862,662,656` bytes available (`df -B1 /`).

## Follow-up cleanup

After the first inventory, the following additional user-owned generated
material was removed:

- superseded VS Code ChatGPT/Claude extension download caches (the newest
  versions were retained);
- completed `/tmp/ndnsf-python-*` and `/tmp/ndnsf-ort120` build temporaries;
- repository and candidate Python `__pycache__` directories and the root
  `.pytest_cache`.

This follow-up recovered approximately 1 GB.  The stale-payload cleanup then
reclaimed another `11,545,866,240` bytes.  The current local status is
approximately `58 GB` available, with `build/` at `4.1 GB` and `results/` at
`6.7 GB`.
The remote candidate SIF remains `4,544,180,224` bytes; there is no local
80-GB SIF.  The Tiger `/project` filesystem reports ample capacity, so a
future SIF build must retain only one sealed candidate plus its manifest and
hash rather than copying model layers into multiple staging directories.
The build cache still references `/tmp/ndnsf-ort120`; it was restored as a
small symlink to `/opt/onnxruntime-1.26.0` after an attempted cleanup exposed
that dependency.  This path is now explicitly protected from cleanup until
the build is reconfigured to a stable non-temporary ORT prefix.

## Follow-up trash and disabled-Snap cleanup

The apparent local pressure was not caused by an SIF.  A second inventory found
two per-revision VS Code Snap trash trees containing already-deleted files:

```text
/home/tianxing/snap/code/254/.local/share/Trash   about 2.0 GiB
/home/tianxing/snap/code/255/.local/share/Trash   about 4.5 GiB
```

`lsof +D` found no open handles in the disabled revision (254).  Its exact
trash files and root-owned remnants were removed, and the disabled Snap
revision was removed with `snap remove code --revision=254`.  In the active
revision (255), every top-level trash entry except `vscode-cpptools` and
`mesa_shader_cache.2` was removed.  Those two directories remain because live
`cpptools`/desktop processes still map files there; their remaining footprint
is about 689 MiB and will be reclaimable after those processes exit.

After this cleanup, `df -B1 /` reported `68,084,424,704` bytes available
(63% used).  The active code Snap remains installed; the candidate SIF remains
the 4.54-GB remote artifact recorded above.  The 594-MB partial Qwen fetch under
`results/spec170-qwen-runtime-v514-lowmem-filter4-20260805T` was deliberately
retained as failure evidence, as were `build/`, manifests, logs, and canonical
results.  Journald was not vacuumed because its recent OOM history is still
needed for diagnosis.

## Follow-up extension-cache cleanup (2026-08-16)

An additional inventory found three old, user-owned VS Code extension copies
with no open file handles.  They were removed file-by-file after the `lsof`
check; the current Claude Code 2.1.233 and ChatGPT 26.810 installations were
retained:

```text
/home/tianxing/.vscode/extensions/anthropic.claude-code-2.1.231-linux-x64
/home/tianxing/.vscode/extensions/anthropic.claude-code-2.1.232-linux-x64
/home/tianxing/.vscode/extensions/openai.chatgpt-26.803.61601-linux-x64
```

This recovered approximately 1.1 GB.  The post-cleanup root filesystem
reported `68,785,836,032` bytes available (`df -B1 /`, 62% used).  No source,
build, result, SIF, manifest, or failure-evidence path was removed.

## Duplicate candidate-release cleanup (2026-08-16)

The temporary candidate worktree contained a byte-for-byte duplicate of the
repository `RELEASE/` payloads.  SHA-256 comparison was performed before
removal; the tracked candidate identity/evidence files were retained.  The
duplicate `RELEASE/` directory was removed, reclaiming about 802 MB.  Failed
Python build scratch and stale root-owned pipeline scratch older than 24 hours
were also removed.  The 594-MB partial Qwen fetch remains intentionally
retained because it is referenced by failure evidence.

After the current Python 3.8 extension was copied and hashed, its 173-MB
temporary compile/link tree was removed as well.  The resulting extension under
`pythonWrapper/ndnsf/` and its ABI evidence remain; no build input is needed
from that temporary path.

## Follow-up generated-result cleanup (2026-08-16)

Before the next SIF build, the repository was checked for active experiment
processes and open handles.  Forty-three exact top-level result directories
whose names identified them as dry runs, smoke runs, retries, debug traces,
stale-health probes, or superseded tail captures were removed with their
root-owned generated files.  Formal results, canonical summaries, manifests,
and failure evidence were retained.  This reclaimed `72,658,944` bytes.

The current local candidate SIF staging remains one base image plus one
overlay; no model payload is copied into the SIF staging directory.

## Post-upload SIF staging cleanup (2026-08-16)

After the final diagnostic SIF was uploaded to Tiger and its remote hash was
verified, the exact local staging paths were removed:

```text
.codex-tmp/spec170-sif-base
.codex-tmp/spec170-sif-overlay-20260816
/tmp/spec170-sealed-source-20260816
```

No open handles were present.  This reclaimed `11,228,884,992` bytes.  The
remote content-addressed artifact and manifest remain at:

```text
/project/tma1/ndnsf-di/releases/spec170-runtime-overlay-20260816-localcpu/runtime.sif
sha256=9aff449aef0884cb68c6b034bdd80a955e549d1a973cbca3b47ef2b55b7a12e2
```

The source seal digest, build identity, and local/Tiger smoke results are
preserved in `overlay-sif-local-20260816.md`; the temporary candidate
worktree remains separately protected for source review.

## Legacy-build and staging cleanup (2026-08-16)

The apparent SIF size concern was rechecked after the upload.  The local SIF
is not present and the verified remote SIF is about 4.8 GB, not 80 GB.  With
no active build, Apptainer, or experiment process, these exact rebuildable
outputs were removed from inactive legacy checkouts:

```text
/home/tianxing/NDN/NFD-Origin/NFD/build
/home/tianxing/NDN/CLF_NFD/CLF_NFD/build
/home/tianxing/NDN/ndn-cxx/build
/home/tianxing/NDN/build
```

Completed `/tmp` staging was also removed, including the superseded source
candidate/seal copies, release bundles, wheel/build temporaries, and finished
pipeline stage directories.  The current NDNSF `build/`, `results/`, protected
candidate worktree, Docker Apptainer image, manifests, and failure evidence
were retained.  This pass reclaimed approximately 4.6 GiB; `df -h /` then
reported about 70 GiB available (59% used).  The remaining `/tmp` footprint is
about 30 MB.

## Journal and diagnostic-temporary cleanup (2026-08-16)

The follow-up storage check found no active NDNSF, Apptainer, Docker-build, or
MiniNDN process.  The verified remote SIF remains approximately 4.8 GB; no
local SIF or SIF staging directory is present.  Exact completed diagnostic
directories were removed from `/tmp` (superseded Gate-B traces and Python
extension build outputs), and stale worktree metadata for the already-missing
`/tmp/spec170-source-candidate-20260816` worktree was pruned.  The protected
`.codex-tmp/spec170-candidate-worktree` was not removed because it contains
uncommitted candidate evidence.

The systemd journal, which was the largest unrelated consumer, was vacuumed
from 2.7 GiB to about 528 MiB with `journalctl --vacuum-size=512M`.  Project
source, current `build/`, canonical `results/`, release artifacts, Docker's
Apptainer image, and all manifests/evidence were retained.  After cleanup,
`df -h /` reports approximately 90 GiB available (47% used).

## Legacy CCLF build cleanup (2026-08-16)

The post-cleanup inventory found no active compiler, NFD, MiniNDN, Docker, or
Apptainer process and no open handles in two separate legacy CCLF checkouts.
Their source trees, Git metadata, and uncommitted source changes were kept;
only rebuildable generated outputs were removed:

```text
/home/tianxing/NDN/NFD-for-cclf/NFD/build          1,012 MiB
/home/tianxing/NDN/ndn-cxx-for-CCLF/ndn-cxx/build     828 MiB
```

Old project-specific `/tmp` staging directories older than one hour were also
removed after the open-handle check. The current NDNSF `build/`, `results/`,
candidate worktree, release manifests, and remote content-addressed SIF were
not touched. Root filesystem status is now approximately 91 GiB available
(46% used). The verified remote SIF is still about 4.8 GiB, so no local SIF is
occupying or expected to occupy 80 GiB.

## Historical-archive and rebuild-cache cleanup (2026-08-16)

The next inventory found no active NDNSF, compiler, Apptainer, Docker, or
experiment process. The following exact, superseded artifacts were removed:

```text
results/spec160-itiger-multinode-qwen/**/source
results/spec160-itiger-multinode-qwen/**/seals/**/*.tar
results/spec162-itiger-qwen36-generation/**/source
results/spec162-itiger-qwen36-generation/**/seals/**/*.tar
results/spec158-layered-reusable-docker/**/seals/**/*.tar
pythonWrapper/build
third_party/llama.cpp-build
/tmp/spec170-seal-dependency-root-20260816
/tmp/ndnsf-di-minindn-gate-b-fixed9-20260816-WZFeGU
/tmp/ndnsf-di-minindn-gate-b-fixed10-20260816-p8t1PU
/tmp/ndnsf-di-minindn-gate-b-fixed11-20260816-CulyR2
```

Parent result summaries, `seal.json` manifests, checksums, current source
seal/dependency root, and Spec170 Gate-A/Gate-B evidence were retained. The
cleanup reclaimed approximately 1.1 GiB of rebuildable/duplicate data. The
remaining `/tmp` usage is about 1.2 GiB, consisting primarily of the protected
current Spec170 candidate source tree (`/tmp/spec170-source-sealed-20260816-c`)
and its 20--21 MiB dependency/seal metadata; these are required for the next
preflight and must not be removed. `df -h /` reports about 91 GiB available.
No local `.sif` file exists; the remote content-addressed SIF remains about
4.8 GiB, so the next SIF build should be bounded and cannot plausibly consume
80 GiB by itself.

## Failed GPU OCI retry cleanup (2026-08-16)

The bounded GPU OCI retry stopped before producing an image. Its failure was
Docker BuildKit `unlazy requires an applier`, not disk exhaustion. With no
build process active, the failed-build-only foundation builder image
(`localhost/spec170/foundation:spec170-2852375-builder`, 5.52 GiB) and unused
BuildKit cache (about 402 MiB) were removed. The final foundation image,
Apptainer image, current source seal, exact dependency worktrees, candidate
worktree, and retry log were retained. Superseded duplicate seal directories
and release zip copies under `/tmp` were removed. `df -h /` then reported
approximately 91 GiB available (47% used); no local SIF exists.

## Failed-seal and stale-worktree cleanup (2026-08-16)

While Tiger job `195965` was still running, a path-whitelisted cleanup removed
the failed 916 MB v3 workspace archive, the superseded 94 MB v1 source staging,
the empty v2 staging, the broken 19 MB exact-dependency worktree, and the
205 MB stale `.codex-tmp/spec170-candidate-worktree` copy. The protected v4
source seal, current candidate worktree, `/tmp/workspace.tar` link, remote
build, and canonical result directories were retained. The stale
`.codex-tmp/spec173-toolchain` copy remains as a small historical build cache;
the stale candidate copy was fully removed after deleting two root-owned
bytecode files with the exact path whitelist. Neither cache is used by the
active build.
After cleanup, the root filesystem reports about 91 GiB available (47% used).
At this historical timestamp there was still no local SIF; the remote build was
then the only SIF-producing step. This statement is superseded by the
2026-08-17 local-SIF-first release record above and must not guide new work.

## Superseded SIF and source-copy cleanup (2026-08-16)

The r4 Tiger build (`196296`) was checked first and remains the only active
Spec170 build. No open handle referenced the seven older runtime images. The
following exact remote SIF files were removed; their release directories,
manifests, hash sidecars, wrapper scripts, and campaign evidence remain:

```text
/project/tma1/ndnsf-di/releases/spec170-local-sif-2c5fa-v16/runtime.sif                         4,547,551,232 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-dev-overlay-2c5fa-native-ort120-v24-full-gates/runtime.sif 4,547,563,520 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-dev-overlay-2c5fa-native-ort120-v25-assignment-fix/runtime.sif 4,547,600,384 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-dev-overlay-2c5fa-native-ort120-v26-trace-lock/runtime.sif 4,547,633,152 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-dev-overlay-2c5fa-native-ort120-v27-role-serial/runtime.sif 4,547,444,736 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-e7ed-ort1200-svs1d432-20260816/runtime.sif 4,544,180,224 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-overlay-20260816-localcpu/runtime.sif 4,786,667,520 bytes
```

The recorded SHA-256 values were, in order:

```text
603c373955b159a7867ae366e255ee79d26ef807310325f9800d0ee9cbd5ba05
d1101e25bce825cc00e850ad03040ddd8b7e7fb8b4c92da781a908b90cd2fe
febf907b886d76a9cae221cfc190d2055bd54fdd33954267c396ef4ede96128e
85ea8a31d84f19198c8f369e67312d736b3ed3a76260192821ca497c305c1038
1c658166028569af4a8b570016f36923795f02139de70e68d13f65b46277e68f
505518e21288eb17b0c4a98c86aa34a4f5e051984d5d3a6942e066b375c9c848
9aff449aef0884cb68c6b034bdd80a955e549d1a973cbca3b47ef2b55b7a12e2
```

This released approximately `32,068,640,768` bytes (29.87 GiB) on the Tiger
project filesystem. The current r4 candidate is not one of these paths and
remains protected until its job and Gate-C evidence close.

The two inactive local source-copy directories from superseded r2/r3 staging
were also removed after an `lsof +D` check:

```text
/tmp/spec170-rootless-source-20260816-v4
/tmp/spec170-rootless-source-20260816-v5
```

The current candidate checkout (`/tmp/spec170-source-sealed-20260816-c`),
current v7 seal/archive, r4 remote staging, current build, results, and all
failure evidence remain protected.

## Tiger obsolete-artifact cleanup (2026-08-16)

The Tiger inventory showed that the large storage consumer was not the current
Spec170 SIF.  The current r5 SIF is `4,397,842,432` bytes with SHA-256
`490ff5fbf20ef3be56caf398478f457efc2a786fdeaf41e90d1ccbbc9addafb6`.
There were no active jobs for `tma1`.  The following exact, unreferenced or
superseded paths were removed after recording their sizes in the remote
manifest `/project/tma1/ndnsf-di/diagnostics/storage-cleanup-20260816T181719Z.txt`:

```text
/project/tma1/ndnsf-di/artifacts/spec162/qwen36                         107,624,989,524 bytes
/project/tma1/ndnsf-di/artifacts/spec162/qwen3-0.6b                       3,029,878,505 bytes
/project/tma1/ndnsf-di/candidates/spec170-v25-assignment-fix                447,840,518 bytes
/project/tma1/ndnsf-di/candidates/spec170-v26-trace-lock                   333,125,908 bytes
/project/tma1/ndnsf-di/candidates/spec170-v27-role-serial                   333,022,931 bytes
/project/tma1/ndnsf-di/candidates/spec170-runtime-e7ed-ort1200-svs1d432-20260816
                                                                            153,478,712 bytes
/project/tma1/ndnsf-di/releases/spec170-runtime-dev-overlay-2c5fa-native-ort120-v24-full-gates/runtime.sif.sha256-link
                                                                          4,547,563,520 bytes
```

The cleanup released `116,469,899,618` bytes (`108.47 GiB`) of old project
payload.  The rebuildable `/home/tma1/.apptainer/cache` was also emptied; its
remaining usage is 26 KiB.  The newer Spec168 model artifacts, current r5
SIF release, release hashes, source seals, manifests, and campaign
evidence were not deleted.  The r5 SIF hash was rechecked after cleanup and
still matches the acceptance record.  The remote project filesystem has about
`840 TiB` available; the local root filesystem has about `91 GiB` available.

After independent exact-SIF verification job 196669, the duplicate r5
`runtime.oci.tar` (`4,718,043,136` bytes, SHA-256
`bf2c73d5086bbec15d4a6c2e85e0e6d7bed136c2d75dfa42aa93cf378dd96422`) was
retired.  Its digest is preserved in the release's `RETIRED-SHA256SUMS` and
`SHA256SUMS.with-retired-oci`; the release now retains only the executable
`runtime.sif`, whose `SHA256SUMS` check passes.

## Local failed-staging cleanup (2026-08-16)

The failed v8 source staging and its one-off archive-comparison directory were
not release artifacts and were safe to recreate.  After checking that the
active candidate checkout and v7 source staging were outside these paths, the
following exact temporary directories were removed:

```text
/tmp/spec170-rootless-source-20260816-v8.1yyRef
/tmp/arch.UKM8sC
```

This released approximately `1.8 GiB` locally.  The current r5 SIF, source
seal, v7 staging needed for the next candidate, and all experiment evidence
remain in place.  Local root availability increased from about `89 GiB` to
`91 GiB`; no SIF or current release was deleted.

The superseded v6 source staging (`/tmp/spec170-rootless-source-20260816-v6`,
`76,605,440`-byte workspace archive) was then removed separately after
confirming that no process referenced it; v7 is the only retained local source
archive for the next candidate build.

## Tiger Qwen/staging cleanup (2026-08-16)

The Tiger inventory confirmed that the apparent large image was not an SIF.
The latest r7 executable SIF is `4,397,907,968` bytes; the retired r6/r7 OCI
archives were intermediate export files, not runtime images.  No `tma1` Slurm
jobs were active.  The exact retired model trees were:

```text
/project/tma1/ndnsf-di/artifacts/spec168/qwen36       53,812,382,796 bytes
/project/tma1/ndnsf-di/artifacts/spec168/qwen3-0.6b    1,514,882,105 bytes
```

The Qwen3.6 tree was an old Spec168 Qwen3.6-27B staged payload, and the small
tree duplicated the content-addressed source retained under
`models/source/qwen3-0.6b`.  Metadata-only archives and checksums were kept at
`/project/tma1/ndnsf-di/diagnostics/storage-cleanup-20260816T200000Z`; the
model payloads were removed.  The two intermediate OCI exports were also
retired only after their SIF hashes passed verification:

```text
r6 runtime.oci.tar  4,718,389,760 bytes  sha256:4ac17217c3390eb34ca99f3732fe11d103866e5c01b8c711bb45e2bc75c89905
r7 runtime.oci.tar  4,716,875,264 bytes  sha256:a90509ceeb52e67919f7798ee24981b076179c75a9baea1240f9efe0faa43101
```

Their hashes remain in each release's `RETIRED-OCI-SHA256SUMS` and
`SHA256SUMS.with-retired-oci`; only the executable SIF and current release
metadata remain.  Superseded Spec170 staging copies were removed as well,
reclaiming `5,464,982,053` bytes; only the r7 sealed source, r7 tools, and the
current D0 bundle remain in staging.  Tiger `/project` still has about `840 TiB`
free and the current release directory is about `13 GiB`, so the SIF cannot
plausibly account for an 80-GB increase.

The three duplicate local rootless source copies
`/tmp/spec170-rootless-source-20260816-v7`, `v8.JWFbME`, and `v9.FygPAs`
were removed after an open-handle check.  The protected v10 archive and current
candidate checkout remain for the next preflight.  Local `/` remains at about
`91 GiB` free and `/tmp` is about `1.3 GiB`.

## Legacy NFD build cleanup (2026-08-16, follow-up)

An inventory found no active NFD, compiler, Apptainer, Docker, or experiment
process and no open handles under the separate legacy checkout
`/home/tianxing/NDN/NFD/build`. It contained only generated compiler outputs
and binaries; the NFD source tree and its uncommitted source changes were
retained. The exact rebuildable directory was removed:

```text
/home/tianxing/NDN/NFD/build
bytes=1,755,529,587
```

After removal, `df -h /` reported about 93 GiB available (46% used). The
current NDNSF `build/`, canonical `results/`, protected candidate source,
Tiger release SIF, manifests, and failure evidence were not touched.

## Local duplicate-source cleanup (2026-08-17)

After the local-SIF candidate and its source seal were durable, an open-process
check found no builder or Apptainer process using the following superseded
temporary source copies. They were moved as exact paths to the user's trash
(recoverable; not hard-deleted) to prevent accidental reuse:

```text
/tmp/spec170-source-sealed-20260816-c
/tmp/spec170-source-sealed-20260816-d
/tmp/spec170-source-sealed-20260816-d.AlePGm
/tmp/spec170-source-sealed-20260816-e.oO7uvp
/tmp/spec170-source-sealed-20260816-f.Jnn24X
/tmp/spec170-source-sealed-20260816-f-workspace-unused.2110774.bak
/tmp/spec170-rootless-source-20260816-v10.fMcB46
```

Their measured aggregate size was `3,783,699,741` bytes. The active local SIF,
`/tmp/spec170-source-sealed-20260816-g1`, `/tmp/spec170-source-stage-20260816-g1`,
source seal, project release, and all evidence remain. The trash was not emptied
in this step, so recovery remains possible.
