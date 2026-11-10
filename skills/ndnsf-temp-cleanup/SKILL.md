---
name: ndnsf-temp-cleanup
description: Audit and reclaim NDNSF temporary disk space, disposable experiment workspaces, retired checkouts and regenerable caches while preserving active deployments, models and evidence. Use for disk-usage investigations, archive-before-delete requests and authorized cleanup; not for uninstalling applications or deleting research history.
---

# NDNSF temporary-space cleanup

Deliver measured free space and a reviewable record of what was removed, archived or retained.
Use the current checkout's rules and actual references. This skill never makes an old path list
or a past cleanup authorization valid for a new run.

## Scope and inventory

- Distinguish analysis-only from authorized deletion. Continue authorized cleanup without asking
  again for each candidate. Installing or reading this skill does not authorize deleting files.
  “Do not remove anything in use” includes retained current work and dependencies even when no
  process currently has them open. Uncertain candidates stay outside the deletion allowlist;
  proceed with independent candidates already established as safe.
- Resolve the repository root and inspect worktree/index state. Read the active feature pointer
  and relevant task/deployment references to identify current work; no new Spec is required for
  housekeeping. Do not build, launch experiments or stop applications merely to clean space.
- Capture `df -B1`, `statvfs` available bytes and same-filesystem `du -x -B1`. Inspect both `/tmp`
  and `/var/tmp`. Use privileged read-only inventory when permissions hide root-owned data;
  do not infer absence or zero size from permission errors. Check deleted-open files with
  `lsof +L1` when `df` and allocated-file totals differ materially.
- Report parent/child overlap and hard links explicitly. Count allocated blocks by unique
  `(device, inode)`; reclaimable blocks require all links to be selected and no live open file.
  Do not add overlapping directory totals or treat logical model sizes as independent copies.
  `/dev/shm`, RAM, swap, filesystem reserved blocks and ordinary disk free space are distinct.

## Classify and verify candidates

Use three outcomes: **remove regenerable idle material**, **archive necessary evidence then
remove disposable material**, or **retain active/unique/uncertain material**. Age, a `tmp` path,
an old Spec number, clean Git status or no open files alone do not establish obsolescence.

Before selecting a path, check its owner and real path, symlinks, nested mounts, process
cwd/exe/fd/maps, open files, workspace locks and current launcher/config/task references.
Recheck before mutation. Avoid printing command lines or configuration secrets unnecessarily.
Use CodeGraph for code references only when an index exists, then confirm against source.

- Preserve current models, shared content caches, original Repo Data, active build/install
  prefixes, credential stores and minimum failure-reproduction inputs. A directory named for
  an older Spec can still supply the current feature's executable or warm-cache baseline.
- For each data purpose, retain only the latest usable version: once a newer SIF/cache/release
  manifest is qualified, old candidate SIFs (about 4.3GB each), old caches and superseded
  intermediates go through an explicit allowlist; keep build records, runtime definitions and
  evidence as identity history. `df` free space is not the user/project quota; use the actual
  write failure (`EDQUOT`/`Disk quota exceeded`) plus directory occupancy to locate the
  boundary, and run a bounded write probe after reclaiming.
- Treat `docs/`, `specs/`, existing archives and Codex sessions/databases/backups as durable
  history. Never infer permission to prune them from a general temporary-space request.
  Trash may contain unique source, reports or failure traces; inspect original paths and
  contents before classifying individual items. Never empty all Trash as a shortcut.
- For Git worktrees, inspect complete status including untracked files and pin HEAD/branch.
  Check current build/deployment references and active use. Preserve the commit/ref and report;
  remove an eligible checkout through `git worktree remove` without force. Record any local
  source links that cease working and how the fixed commit can restore the source.
- For pip/uv, use their maintained cache commands without forcing through in-use protection.
  Check cache-backed environments/interpreter links and active package operations first.
  Keep installed environments, models and managed Python interpreters outside the cache scope.
- VS Code installation-package cache is separate from installed extensions/configuration.
  Prefer explicit old, unchanged, unopened cache files; preserve recent/incomplete downloads.
  For Snap downloads, require no active change and target only verified cache entries. Exclusive
  old regular files provide a conservative first batch; shared hardlinks do not promise space.
  Do not touch installed snaps, seed or mounted revisions. Revision removal or journal retention
  requires a concrete policy in the current request, not an invented blanket retention window.

## Archive and remove

1. Create an explicit absolute-path allowlist and a compact plan outside the cleanup targets.
   Record path identities, type, device/inode, owner, mode, size, mtime and link count; record
   content hashes for evidence to preserve. Do not follow symlinks into other trees.
2. For managed disposable workspaces, reuse the maintained workspace lock/marker and
   live-user/mount checks when available. Hold the exclusive slot lock through recheck,
   archival and deletion; preserve the slot marker/lock and leave an empty reusable `work/`.
   If helpers are absent, implement equivalent bounded checks for this batch, not a new
   general cleanup framework. Root ownership is not permission for broader recursive deletion.
3. Preserve commands, source/config identity, outcomes, first failure, readable timing tables
   and structured measurements needed by the existing report. Keep unresolved failure subsets.
   If historical statistics have not been verified, retain the necessary raw measurement data
   in a compact archive and say so. Do not claim aggregates support arbitrary raw-data reanalysis.
4. Archive source/config/log/model inputs or unique unknown files as needed; discard compiled
   products only after establishing they can be regenerated and are not required current inputs.
   Do not copy whole temporary trees into evidence just to move their consumption elsewhere.
   Check actual destination headroom before compression; never stage bulk data in `/dev/shm`.
5. Verify archives by fully reading members and checking names, hashes and link metadata before
   deleting originals. Record the final archive hash. Restrict archives containing test identities
   or credentials (normally directory 0700/files 0600). Never follow archive symlinks when checking.
6. Revalidate selected file identity/content before removal; reject changes. Delete only the
   allowlisted entries and empty directories. Stale Unix sockets can be removed after proving
   inactivity and retaining endpoint identity; unexpected special files require investigation.
   Never use blanket `rm -rf /var/tmp`, `git clean`, automatic mount deletion or global `chown`.
7. Keep per-batch checkpoints. On failure retain the first boundary, inspect completed receipts
   and resume only remaining verified work. Never rerun a historical one-shot cleanup blindly.

## Completion

Independently check archive hashes, expected path absence/empty workspaces, retained markers
and current required paths. Record skipped items and reasons; path existence is not bytewise
preservation or functional qualification. Measure final available bytes and target allocation.
Report net free-space gain separately from removed directory size: archives on the same disk,
external hard links, open files and concurrent system writes can make them differ.

Keep one readable report plus machine-readable manifests/receipts in the existing `results/`
or active evidence location, without secrets in chat. State archive/raw-data limits and checks
not run. Do not commit or push unless requested. Prevent recurrence by reusing stable locked
purpose workspaces and keeping durable inputs/evidence outside scratch.

For the concrete 2026-11-07 case and known pitfalls, read
[references/2026-11-07-case.md](references/2026-11-07-case.md) when useful. Its sizes and paths
are historical evidence, not current inventory or an executable deletion list.
