# Repository agent instructions

## Portable authority and scope

- This tracked file contains the minimum repository-wide rules. Resolve all
  relative paths from this checkout, not another machine's home directory.
  User instructions and higher-priority agent instructions take precedence.
- Except for the applicable bundled skills, no other file is required to define
  or interpret these agent rules. There is no instruction include/import chain
  through a README, design document, constitution, Spec or personal agent file.
  References below identify task inputs, output locations or optional tools;
  they do not delegate the rules to those files. Actual code, manuscript sources
  and task-specific evidence are still needed to perform the corresponding task.
- NDNSF owns service discovery, participation feedback, selection and invocation.
  UAV and distributed inference applications own task semantics and selection
  policies. DistributedRepo stores and retrieves original signed Data; it must
  not become a second execution or authorization authority.
- Use Waf to configure, build and install this repository's targets. External
  dependencies use their own maintained build/install systems. MiniNDN runs
  consume system-installed libraries and applications, not ad hoc copied binaries.
- Respect analysis-only requests. Do not launch builds, MiniNDN, SIF construction
  or cluster jobs merely to revise documentation or inspect source. Distinguish
  static review, build success, focused tests and end-to-end qualification.
- Preserve unrelated worktree and index changes. Stage explicit authorized paths;
  never use `git add -A` or destructive resets. Do not commit or push unless asked.
- Local `specs/`, `.specify/`, `.agents/`, evidence and ignored documentation are
  optional checkout context, not prerequisites for understanding these rules.
  For Spec-based work, if present, `.specify/feature.json` selects the active
  feature; check its plan and tasks before implementing that feature.
  Follow specification -> plan -> tasks ->
  implementation -> verification. If required task-specific contracts or evidence
  are missing, report that gap rather than inventing requirements or completion.
- The managed plan pointer below is local context, not a declaration that its
  files exist in a fresh clone or that its feature is complete. Do not infer the
  active feature from this pointer when a newer local feature pointer exists.
  Ignore an unavailable pointer for unrelated tasks; it is not a startup gate.
- Paths below describe supported tooling or optional manuscript work. Check that
  a referenced file exists before using it; its absence must not block unrelated
  repository work. Never recreate private baselines or experiment evidence.

## Bundled skills: install missing only

- The versioned `skills/` directory contains the portable skill packages and
  their bundled references/scripts. Read the applicable `SKILL.md` before use.
  Skills do not override this file or current user authorization.
- At first use on a machine, check/install missing packages with:

  ```bash
  # Run from the repository root. This copies missing packages only.
  python3 - <<'PY'
  import os
  from pathlib import Path
  import shutil

  source = Path.cwd() / 'skills'
  if not (Path.cwd() / 'AGENTS.md').is_file() or not source.is_dir():
      raise SystemExit('Run from the NDNSF repository root')
  root = Path(os.environ.get('CODEX_HOME') or str(Path.home() / '.codex'))
  destination = (root / 'skills').expanduser().resolve()
  if destination == source.resolve() or source.resolve() in destination.parents:
      raise SystemExit('Personal skills destination must be outside bundled skills')
  destination.mkdir(parents=True, exist_ok=True)
  for package in sorted(source.iterdir()):
      if not package.is_dir() or not (package / 'SKILL.md').is_file():
          continue
      target = destination / package.name
      if os.path.lexists(target):
          print('SKIP existing:', target)
          continue
      if package.is_symlink() or any(p.is_symlink() for p in package.rglob('*')):
          raise SystemExit('Refusing package with symlinks: ' + str(package))
      try:
          target.mkdir()  # Exclusive claim; do not overwrite a concurrent install.
      except FileExistsError:
          print('SKIP existing:', target)
          continue
      try:
          shutil.copytree(package, target, dirs_exist_ok=True,
                          ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
      except Exception:
          print('Incomplete copy retained; inspect manually:', target)
          raise
      print('INSTALLED:', target)
  PY
  ```

  The destination is `$CODEX_HOME/skills`, or the current user's
  `~/.codex/skills` when `CODEX_HOME` is unset. No network, sudo or third-party
  installer is needed. If copying fails, report the incomplete target; do not
  automatically delete it or overwrite it on the next run.
- Existing names, including symlinks, are always skipped: do not update,
  synchronize, overwrite or delete an existing personal skill automatically.
  Report incomplete installations or version differences; use the versioned
  repository package explicitly when necessary and permitted by the client.
  An actual personal-skill update needs separate user authorization.
- A client may require a new session/reload to discover newly installed skills.
  Installation alone does not enable external services, provide Spec Kit command
  entrypoints from `.agents/skills/`, or prove product/test readiness.

## Design, implementation and verification

- Before a substantive change, identify the real caller, owner, inputs, outputs,
  state transitions, compatibility path and failure cleanup. Use existing APIs
  and maintained launchers rather than introducing parallel implementations.
- A design must state the observable result, an independent acceptance check and
  relevant negative/recovery cases. Task progress is not verification evidence.
  Check actual source before relying on historical documents or cached context.
- Prefer small, coherent work units with one behavioral outcome. Do not split
  ordinary coding, documentation and evidence into unrelated administrative tasks
  or accumulate unrelated changes merely to save a build.
- NDNSF-DI native behavior tests must exercise production C++ targets with C++
  fixtures and assertions. Python may orchestrate processes or test bindings, but
  cannot replace native behavior or cross-process end-to-end qualification.
- Preserve asynchronous dependencies until workers finish; inspect callback
  ownership, shutdown and cleanup. Do not weaken production semantics to make a
  fixture pass or suppress leaks instead of diagnosing them.
- Record actual commands, source/configuration identity, outcomes and first
  failure boundaries. A successful CLI help, import, build or local smoke test
  cannot be reported as successful inference or distributed qualification.
- On failure, distinguish source, build/link, runtime, harness and resource
  boundaries. Revise the affected check before retrying; do not silently change
  model, resource limits, timeout or workload to manufacture a passing result.
- Use the bundled `speckit-code-design` for detailed design/task review,
  `ndnsf-minindn-experiment` for MiniNDN script work, `itiger-ndnsf-ops` for
  authorized SIF/Tiger work, and `review` for explicitly requested diff reviews.
  Detailed skills supplement these self-contained rules; unavailable local
  Spec Kit entrypoints or evidence must be reported, not fabricated.

## NDNSF-DI experiment storage

- Reuse a stable disk workspace per purpose rather than creating a fresh
  model-sized build/staging directory for every retry. When available, prefer
  `python3 scripts/ndnsf-di-workspace.py PURPOSE -- COMMAND {workspace}`;
  it resets only its marked scratch directory under an exclusive lock.
  Commands run inside that directory and receive `TMPDIR` and
  `NDNSF_DI_WORKSPACE`. Use absolute command/source paths. This bounds retained
  scratch history to the latest run per purpose, not a per-run byte quota.
- Keep model sources, shared content-identity caches, and durable experiment
  logs/receipts outside scratch. Qwen's default `--fixed-work-root` already
  reuses one locked workspace; explicit `--run-root` is durable evidence.
  YOLO's default result/cache paths are stable; use a fixed `--results-dir`
  for disposable retries and archive required results separately.
- Do not place bulk build objects, install snapshots or model data in
  `/dev/shm`. Its capacity is also needed by VS Code/Chromium. Reuse disk
  workspaces for these operations, including failed retries. Never purge
  another active workspace, mounted trees, source models or evidence roots.
- If the local workspace helper is unavailable, use an explicitly owned, stable
  disk scratch directory with exclusive access; keep source/cache/evidence
  outside it. Never replace this constraint with per-retry bulk staging in RAM.

## Host build stability

- For host compilation, use two jobs, a bounded compiler memory budget
  (default 6.5 GiB) and a preflight check for at least 2 GiB desktop headroom.
  Keep compiler processes under the invoking user in an isolated resource group.
  Preserve `WAFLOCK` when using an existing configured build directory.
  When available, `scripts/build-safe.sh build --jobs=2` implements this policy.
  This optional launcher
  runs compiler processes as the invoking user in a separate systemd service
  with a default 6.5 GiB memory limit and a startup check for 2 GiB desktop
  headroom. Kernel OOM events within this service stop the build instead of
  selecting VS Code as their victim. The measured standalone
  ServiceProvider sanitizer compile exceeded the initial 6 GiB limit.
- Keep the Waf semaphore that compiles source units of at least 128 KiB
  exclusively, including `ServiceUser.cpp` and `ServiceProvider.cpp`;
  smaller units retain two-way parallel compilation. Do not raise the memory limit or job count during an
  editor instability investigation without measuring available headroom.
- The safe-build helper may be local-only. If absent, arrange an equivalent
  bounded Waf invocation using available OS resource controls before compiling;
  no particular helper file is mandatory. Do not silently fall back to an
  unrestricted parallel build. Reading or editing source needs no build launcher.

<!-- SPEC_KIT_PLAN_START -->
Active implementation plan is at specs/201-producer-data-naming/plan.md.
<!-- SPEC_KIT_PLAN_END -->

## Git boundary for documentation and specs

- Treat `docs/` and `specs/` directories as local-only material, with one user-authorized exception: `docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/`. Its paper source, bibliography, and figure assets may be tracked in Git. Generated manuscript PDFs and compilation intermediates remain ignored.
- Preserve the ignore rules for all other documentation and specs. Do not broaden the paper-source exception or force-add unrelated files.
- Put durable implementation changes in source code and tests. Keep planning and Spec Kit artifacts in the local `specs/` tree.

## Data naming and transparent Repo

- Every published Data packet must use the naming structure `<actual Producer's complete prefix>/<data category>/<identity and version information>`. This applies to business payloads, manifests, segments, control replies, ACKs, status records, and transport Data envelopes. Preserve standard protocol naming conventions beneath the authorized producer prefix.
- The Producer is the actor that generates and originally publishes the content. Use its complete prefix and validate the signer's authorization for that namespace. Never substitute a global namespace, an inferred rank-only prefix, or another actor's prefix; fail explicitly if the Producer cannot be resolved.
- Repo is transparent storage and retrieval: preserve the original Data name, content, metadata, signature, and wire packet. Do not rename, re-sign, re-encode, re-encrypt, wrap, or otherwise transform stored Data. Fetch hints and replica locations belong in references/routing metadata, not in replacement content names.
- Repo-generated management replies and inventory records belong under the Repo's own producer prefix; they must reference stored objects by their original producer names.
- Manifests may reference objects from different Producers. Each referenced object retains its own complete producer name. Coordinate names and authorization, retrieval, validation, and cache keys together when changing a naming format.
- Use stable content identities for reusable immutable objects and explicit execution bindings for invocation outputs. Reuse unchanged objects without request-specific renaming; never reuse a name for changed content or changed protection parameters. Keep C++ and Python component types and wire encodings identical.

## NDNSF paper source and compilation

- The NDNSF paper source copied from Overleaf is maintained in `docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/`. Its entry point is `NDNSF.tex`, chapter files are in `sections/`, and the bibliography is `sections/references.bib`. Preserve relative figure and bibliography paths; do not create a competing source tree or edit generated PDFs directly.
- This is the user's older manuscript, distinct from the dissertation proposal in `docs/PAPER/proposal-defense/`. Do not silently substitute newer implementation claims. Compilation-only requests do not authorize manuscript revisions.
- Use `latexmk` with pdfLaTeX (IEEEtran and BibTeX), running from the source directory. From the repository root:

  ```bash
  cd docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework
  latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=../.overleaf-build NDNSF.tex
  ```

- Keep auxiliary files in `.overleaf-build/` beside the source directory. After successful compilation, inspect `../.overleaf-build/NDNSF.log` for errors and unresolved citations/references, and check the PDF with `pdfinfo`.
- Publish the verified build as `docs/PAPER/named-data-network-service-framework-paper/NDNSF.pdf`, by copying `../.overleaf-build/NDNSF.pdf` to `../NDNSF.pdf` from the source directory. Replace only the known generated deliverable, never an unrelated/user-supplied PDF or a good output with a failed/stale build. Preserve an existing destination of uncertain provenance.
- For content/layout changes, render and inspect affected pages as well. Track the authorized paper source and figure assets under the Git exception above; keep generated outputs local-only.

### Fixed baseline and marked comparison

- Keep `docs/PAPER/named-data-network-service-framework-paper/NDNSF_Base/` as the fixed, read-only comparison baseline supplied by the user. Do not edit, replace, or regenerate its sources unless the user explicitly requests a baseline change. It remains local-only, outside the Git source exception.
- Make all manuscript revisions in `docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/`, never in the baseline or generated comparison TeX.
- Preserve the contribution heading `Reusable Identity Credentials with Stable Trust Rules` when that contribution is introduced; do not rename it to “One unified Credential.” This heading does not waive the need to substantiate security claims.
- After authorized manuscript revisions, build the clean PDF with the direct
  `latexmk` command above and the comparison PDF against the supplied baseline.
  When present, the following convenience script performs both builds:

  ```bash
  bash docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/build-paper.sh
  ```

- The comparison procedure expands the manuscript's `\input` files and runs
  `latexdiff` against `NDNSF_Base/NDNSF.tex`. Available `make-comparison.py` and
  `diff-preamble.tex` files implement this procedure but do not define additional
  agent rules. Changed tables are preserved as whole old/new tables rather than
  mixing incompatible column layouts. Additions are yellow (RGB 210,165,0 for
  readability on white); deletions are red (RGB 210,0,0). Unchanged text retains
  its normal appearance. Keep the original font/size, with no strikeout,
  underlining, side-by-side columns, or manual markup in the clean manuscript.
- If the convenience files are absent, apply the procedure and formatting above
  with available LaTeX tools. If manuscript sources or the user-supplied baseline
  are absent, report the missing task input; do not invent or fetch a replacement
  baseline. None of these manuscript files is required for unrelated code work.
- Deliver `NDNSF.pdf` (clean revised paper) and `NDNSF_comparison.pdf` (marked comparison) directly in `docs/PAPER/named-data-network-service-framework-paper/`. Build intermediates stay in `.overleaf-build/` and `.comparison-build/`, respectively. These outputs are local-only and must not be staged.
- Inspect changed pages for rendering errors before claiming review completion. Text diff markup is not a complete audit of changed figure binaries, bibliography database contents, or preamble/layout changes; inspect and report those separately. If baseline and revised manuscript are identical, the comparison legitimately has no colored changes. Never invent edits to demonstrate markup.

## CodeGraph

In repositories indexed by CodeGraph (a `.codegraph/` directory exists at the repo root), reach for it BEFORE grep/find or reading files when you need to understand or locate code:

- **MCP tools** (when available): `codegraph_explore` answers most code questions in one call — the relevant symbols' verbatim source plus the call paths between them. `codegraph_node` returns one symbol's source + callers, or reads a whole file with line numbers. If the tools are listed but deferred, load them by name via tool search.
- **Shell** (always works): `codegraph explore "<symbol names or question>"` and `codegraph node <symbol-or-file>` print the same output.
- If there is no `.codegraph/` directory, skip CodeGraph entirely — indexing is the user's decision.

## Transient capacity and service failures

Do not abandon an active task because of a transient model-capacity, rate-limit,
transport, stream, or service-unavailable error. When the error is delivered to
an active agent, preserve current task state, retry the same operation with
bounded backoff, and resume from the last verified checkpoint instead of
restarting the task. Use a supported fallback model automatically only when
the Codex host exposes an explicit fallback mechanism that preserves the thread
and task context. Report a blocker only after bounded retries fail.

This instruction cannot intercept failures that occur before a model turn is
created. For those host-side failures, the Codex IDE/CLI itself must provide
retry or model fallback; never claim that an agent instruction can catch them.
