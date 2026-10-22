# NDNSF project handbook for agents

## Portable authority and scope

- This file contains the project context, rules and essential workflow needed
  to start work. Keep it self-contained: do not replace a rule or architecture
  explanation with an instruction to read another document. Code paths below
  locate implementations; they do not define additional agent rules.
- Keep durable conventions, supported commands and protected paths here. Put
  task plans, progress, experiment results and debugging history in their
  existing task/evidence locations. Amend an existing rule instead of adding
  another rule for the same responsibility.
- Resolve relative paths from this checkout. Higher-priority instructions and
  current user decisions take precedence. No README, design document, Spec,
  personal configuration or skill is required to interpret this handbook.
  Relevant source, task requirements and original evidence remain necessary
  inputs to the work; self-contained rules do not replace inspecting them.
- Use Waf to configure, build and install this repository's targets. External
  dependencies use their own maintained build/install systems. MiniNDN runs
  consume system-installed libraries and applications, not ad hoc copied binaries.
- Respect analysis-only requests. Do not launch product builds, MiniNDN, SIF
  construction or cluster jobs merely to revise documentation or inspect source.
  Authorized manuscript revisions include the LaTeX builds specified below.
- Preserve unrelated worktree and index changes. Stage explicit authorized paths;
  never use `git add -A` or destructive resets. Do not commit or push unless asked.
- For Spec-based work, `.specify/feature.json`, when present, selects the active
  feature. Read that feature's plan and tasks; follow specification -> plan ->
  tasks -> implementation -> verification.
  A pointer is not evidence of completion. Do not create a Spec for an unrelated
  small edit merely to satisfy this workflow.
- Local Specs, skills, evidence and tools may be absent in a fresh checkout.
  Check referenced paths before using them. Missing optional context must not
  block unrelated work; report missing inputs needed for the actual task.
  Never invent requirements, private baselines, experiment evidence or completion.

## Project architecture and code entry points

NDNSF is a C++ service framework over Named Data Networking (NDN). A `ServiceUser`
invokes a service; a `ServiceProvider` hosts its application handler; a
`ServiceController` manages service permissions, including grants and revocation.
The framework integrates NDN object validation and NAC-ABE (Name-based Access
Control with Attribute-Based Encryption). Service permission and trust in a
signer's identity are separate checks.

The normal invocation has four logical messages: User Request -> Provider ACK
-> User Selection -> Provider Response. Providers supply participation feedback;
the user selects according to application policy, and selected providers execute
after input and authorization checks. NDNSD is an optional service-information
discovery mechanism, not a prerequisite for Request/ACK participation discovery.

Targeted invocation is supplementary and names a provider in advance. Eligible
calls with the required authorization material can use Request -> Response;
bootstrap or request-specific key establishment may still require ACK/Selection.
Targeted mode does not bypass security checks, and restricting the normal path
to one candidate does not by itself make it targeted invocation. These logical
message sequences are not packet counts or latency guarantees.

NDNSF-DI is the distributed-inference layer: it owns inference planning,
assignments and tensor dependencies, using NDNSF for service invocation and
adapters for backend execution. Repo stores original Named Data and serves it
without becoming its Producer or executing application tasks. Models and
application policy belong to the application/adapters, not generic NDNSF Core.
Production behavior in NDNSF Core, NDNSF-DI, NDNSF-UAV-APP and
NDNSF-DistributedRepo belongs in C++. Python is a thin interface for calling
native APIs, translating inputs and outputs, launching processes, presenting
UI, and preparing or orchestrating experiments. Python must not be the sole
owner of a production protocol, authorization, planning, placement, task or
artifact lifecycle, tracking decision, or Repo storage/repair algorithm.

Use these source entry points, then trace the affected callers and consumers:

| Responsibility | Source entry points |
| --- | --- |
| Invocation, feedback and selection | `ndn-service-framework/ServiceUser.cpp`, `ServiceProvider.cpp` and their adjacent headers |
| Permissions and message protection | `ndn-service-framework/ServiceController.cpp`, `NDNSFMessages.cpp`, `SecurityProfile.cpp`, `HybridMessageCrypto.cpp` and their adjacent headers |
| Native inference lifecycle and placement | `NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.cpp`, `NativeV3Placement.cpp`; backend adapters in `NDNSF-DistributedInference/cpp/adapters/` |
| Repo storage and client operations | `NDNSF-DistributedRepo/src/RepoNode.cpp`, `RepoClient.cpp`; public interfaces in `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/` |
| Bindings, applications and experiments | `pythonWrapper/src/`, `examples/`, `NDNSF-UAV-APP/`, `Experiments/` |
| Build and verification registration | Root `wscript`, `examples/wscript`, `tests/wscript`, `NDNSF-DistributedRepo/wscript`; fixtures in `tests/` |

Paths grouped in one row share the stated directory unless another directory is
given. This map identifies ownership and starting points, not proof that every
path satisfies the rules below. Verify current behavior from source and relevant
evidence before changing it or describing it as implemented.

## Mandatory engineering principles

Apply the relevant rules to design and code changes. If a necessary exception
is not already authorized by the user, explain its conflict, rationale and
impact and obtain approval before implementing it. Do not request the same
approval again or hide an exception in a special-case branch.

1. **Producer-owned, immutable Data identity.** Every published application Data
   packet belongs under its actual Producer's complete prefix, followed by its
   category and identity/version components, including payloads, manifests,
   segments, control replies, ACKs, status records and transport Data envelopes.
   The Producer generates and originally publishes the content, not merely
   stores or forwards it. Validate the signer's authority for
   that namespace; fail if the Producer cannot be resolved. Preserve standard
   protocol naming conventions. Do not use another actor's prefix, a global-only
   prefix or an inferred rank-only identity. Never reuse a name for changed
   content or protection parameters. Immutable reusable objects keep stable
   identities; execution outputs retain their required request/attempt/round
   bindings. Objects referenced by a manifest retain their own Producer names.

2. **Data-driven distributed execution.** Cross-node task assignment, dependency
   delivery and result-driven state transitions use published, retrieved and
   validated Named Data. A computation starts only after its declared input and
   authorization prerequisites are satisfied. Timers and local policies may
   trigger timeout, cancellation or the next publication, but cannot bypass
   dependency admission or silently authorize remote execution. No undocumented
   side channel may bypass this lifecycle. Local computation, device calls and
   private in-process state need not become published Data.

3. **Object- and context-based security.** Apply the same applicable name,
   signature, trust, authorization and execution-binding checks whether an object
   arrives from its Producer, a cache or a Repo. A valid signature alone does not
   confer service permission; decryption does not replace signature validation.
   Reject missing or mismatched required identities, permissions or bindings.
   A trusted connection or cache hit must not bypass object admission.

4. **Transparent Repo.** Store and retrieve original Data without renaming,
   re-signing, re-encoding, re-encrypting, wrapping or modifying its wire packet,
   content or metadata. Put replica locations and forwarding hints in retrieval
   references, not replacement content names. Repo-generated management replies
   and inventories use the Repo's own Producer prefix and reference original
   names. Repo is not an application execution or authorization authority.

5. **One owner and one authoritative implementation.** NDNSF owns discovery,
   feedback, selection and invocation; applications own task semantics and
   selection policies; adapters own backend computation; Repo owns storage.
   Cross these boundaries through explicit interfaces, not private-state access
   or mutable process-global state. Implement complete production behavior in
   C++ across Core, DI, UAV and Repo; keep Python wrappers thin and free of
   independent security, placement, execution or storage decisions. A Python
   entry point must call the authoritative native path rather than duplicate its
   state machine. Reuse maintained APIs and launchers instead of parallel
   implementations.

6. **Owned, bounded lifecycles.** Every asynchronous worker, callback, timer,
   cache, buffer and child process needs an owner, a resource bound or backpressure
   policy, cancellation behavior and a release path. Preserve dependencies until
   work drains; prevent strong callback-reference cycles. Terminal executions
   cannot advance, and retries must not cause uncontrolled duplicate side effects.
   Do not hide failures with unlimited queues, cache growth or retries.

7. **Embedded ndn-cxx-style C++ subset.** Use two-space indentation, no tabs and
   normally at most 100 columns. Types use `UpperCamelCase`, functions/variables
   `lowerCamelCase`, non-public non-static members `m_`, and named constants/enum
   values `UPPER_SNAKE_CASE`. New files use `lower-hyphen.hpp/.cpp`; do not rename
   existing files merely for style. Class/function opening braces occupy a new
   line; control-statement braces stay on the same line. Put return types on a
   separate line in multiline function declarations/definitions. Headers have
   include guards; use quotes for project headers and angle brackets for external
   headers, grouped and sorted, with the corresponding header first in `.cpp`.
   Write English `//` comments and `/** */` interface documentation. Normally mark
   converting single-argument constructors `explicit`; avoid C-style casts.
   Apply this subset to new and edited code without unrelated mass reformatting.
   Reference only (not required reading):
   https://docs.named-data.net/ndn-cxx/current/code-style.html

8. **Production-path changes and proportionate evidence.** Coordinate interface,
   naming and wire-format changes across producers, consumers, validation,
   authorization, retrieval and cache keys; maintain C++/Python component-type and
   wire parity where both implement a contract. Register new targets and install
   outputs in the maintained Waf system. Verify actual production behavior and
   relevant negative/recovery cases; do not substitute an experiment-only bypass
   or a reimplementation of production logic for that evidence. Distinguish
   static review, build, smoke tests and real end-to-end qualification.

9. **Source-first static diagnosis.** Before a substantive code change or expensive
   build/experiment, inspect the actual caller, owner, inputs, outputs, state
   transitions and compatibility path. Source takes precedence over historical
   documents or cached context. Check contracts, serialization, trust and
   authorization, resource ownership, failure cleanup, build/link registration
   and the relevant test/oracle as applicable. Use CodeGraph only when an index
   exists, then confirm against source. Separate confirmed defects, conditional
   risks and runtime questions; static inspection cannot prove runtime success.
   Use small, targeted probes where source alone cannot resolve the question,
   rather than blindly repeating full builds or experiments. After a failure,
   retain the first boundary and revise the affected static check before retrying.
   Keep this review within the existing work unit, not a new report bureaucracy.

10. **Minimum sufficient design; no speculative engineering.** First establish
    whether new code is necessary. Prefer, in order, an existing project path,
    reusable project helper/type, standard library, suitable platform facility,
    or an already-declared compatible dependency; otherwise add the smallest
    maintainable custom implementation that satisfies the contract. Every new
    abstraction, dependency, configuration option, worker, cache, build stage or
    duplicated state must have a concrete current requirement and an owner.
    Reject unused future extension points, parallel launch/build pipelines,
    repeated conversions/copies and unrelated cleanup. Extract shared logic for
    actual repeated behavior or a demonstrated ownership/testability boundary,
    not hypothetical future consumers. Prefer one supported default path and
    explicit configuration over hidden fallbacks and extra knobs. Minimize
    independent concepts, mutable state and maintenance burden, not merely line
    count. Never simplify by removing approved behavior, security checks,
    cancellation/cleanup, necessary observability or required tests. Apply this
    check to the affected diff within its existing review; add no separate gate.
    Keep fixes and performance optimization within NDNSF-owned components,
    including NDNSF-DI. Do not modify external libraries or dependencies such as
    NFD, ndn-cxx or ONNX Runtime unless strictly necessary for the requested
    outcome. Before proposing an external change, establish the need from source
    or measured evidence, explain why supported APIs/configuration and changes
    within NDNSF cannot suffice, and state the affected scope and compatibility
    impact. Follow the exception approval rule above before implementing it.

## Optional skills and installation

- Skills are optional execution aids; the required design, review and manuscript
  checks are stated in this file. Use a skill when it helps the current task,
  reading its `SKILL.md` before use. Do not make skill discovery or installation
  a startup gate, or outsource project requirements to skill documents.
- Available helpers may include `speckit-code-design` for design/tasks,
  `ndnsf-minindn-experiment` for MiniNDN, `itiger-ndnsf-ops` for SIF/Tiger,
  `academic-humanizer` for prose and `review` for changes. If unavailable, apply
  this handbook directly; missing source, tools or evidence needed for the task
  must still be reported.
- Install bundled packages only when requested or needed for a chosen skill
  workflow. Copy missing packages to `$CODEX_HOME/skills`, or `~/.codex/skills`
  when `CODEX_HOME` is unset. Use this checkout's `skills/` as the source after
  confirming the repository root and source directory exist; require a
  destination outside that source tree. Copy
  only directories containing `SKILL.md`; reject source packages with symlinks
  and omit `__pycache__` and `*.pyc`. No network or sudo is needed.
- Skip every existing destination name, including dangling symlinks. Claim each
  new package directory exclusively before copying so concurrent installations
  cannot overwrite each other. On failure, report and retain the incomplete
  target; do not automatically delete, overwrite or retry into it.
- Never automatically update, synchronize or delete an existing personal skill.
  Report relevant version differences and use the bundled package explicitly
  when necessary and supported by the client. Updating an installed skill needs
  user authorization.
- A client may require a new session/reload to discover newly installed skills.
  Installation alone does not enable external services, provide Spec Kit command
  entrypoints from `.agents/skills/`, or prove product/test readiness.

## Design, implementation and verification

- Establish the requested outcome and scope first. Inspect worktree/index state
  and the affected source, then choose the smallest change that closes the gap.
  For Spec work, use the selected feature's requirements, plan and tasks as task
  inputs. For a small direct fix, state the plan in the existing task or response;
  do not create a document hierarchy just to begin work.
- A design must state the observable result, an independent acceptance check and
  relevant negative/recovery cases. Task progress is not verification evidence.
- Execute builds and runtime checks only within the authorized task scope;
  report checks not run. A documentation edit alone does not authorize product
  tests or experiments.
- Prefer small, coherent work units with one behavioral outcome. Do not split
  ordinary coding, documentation and evidence into unrelated administrative tasks
  or accumulate unrelated changes merely to save a build. Implementation tasks
  should change product code or verify C++ behavior; update affected documentation
  in the same task when an interface, behavior or security contract changes.
- Scope each Spec to one concrete outcome; first check whether qualified evidence
  and maintained code already suffice. For every task, name the remaining gap
  and the smallest change or check that closes it; remove speculative variants
  and duplicate document work. Expand an experiment only when its current
  condition cannot answer the stated question, not to seek a favorable result.
- NDNSF-DI native behavior tests must exercise production C++ targets with C++
  fixtures and assertions. Python may orchestrate processes or test bindings, but
  cannot replace native behavior or cross-process end-to-end qualification.
- Record actual commands, source/configuration identity, outcomes and first
  failure boundaries. A successful CLI help, import, build or local smoke test
  cannot be reported as successful inference or distributed qualification.
- On failure, distinguish source, build/link, runtime, harness and resource
  boundaries. Revise the affected check before retrying; do not silently change
  model, resource limits, timeout or workload to manufacture a passing result.

### Review and completion

- Pin the scope and baseline before reviewing: use the requested branch's
  merge-base for a branch comparison, `HEAD` or the requested base for pending
  worktree changes, and the fixed Base for the NDNSF paper. A recorded pre-edit
  snapshot isolates changes made during the current task; it does not cover
  earlier pending work. Include relevant staged, unstaged and new files, as well
  as deletions; a committed-only diff can omit pending work.
- For each coherent change, check its necessity, fit with project ownership and
  context, supporting source/evidence, and whether a smaller change suffices.
  Apply the affected engineering and manuscript rules in this file. Distinguish
  confirmed defects, conditional risks and unverified claims.
- Review-only requests leave files unchanged. When correction is authorized,
  fix supported findings in the maintained source and recheck the affected
  diff. Keep the review within the task, without a new reporting process.
- Before completion, inspect the final diff for unrelated changes and report
  what changed, actual verification performed and material remaining limits.
  Do not claim build, runtime, experiment or publication readiness from static
  inspection alone. Commit and push only when requested.

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
- Finish each experiment, including failed or interrupted runs, by archiving
  its report, statistics and the measurement data needed to recompute and audit
  its results inside this repository's existing `results/` or active feature's
  `specs/` evidence directory. Retain commands, source/configuration identity,
  outcomes, relevant logs and the first failure boundary. Preserve necessary
  per-sample data; aggregate statistics alone are insufficient when they cannot
  substantiate the reported result. Include a detailed timing table for every
  experiment and each request/round, including failed or interrupted attempts.
  Separate launch/material preparation, first request and subsequent requests;
  state timing boundaries and denominators, wall time, available CPU time,
  counts and evidence sources. Keep an additive end-to-end partition separate
  from overlapping/nested step measurements; do not label all non-ONNX time or
  wall-minus-CPU as network time. Mark missing measurements and the first
  failure explicitly. Compare matched conditions and use the measured breakdown
  to select the next optimization before changing code or running another trial.
  Store readable tables and recomputable structured data with the run report.
  Do not copy whole temporary run trees into
  the archive merely to relocate their disk consumption.
- After verifying that the archive is complete and readable and the run's
  processes have stopped, remove that run's disposable artifacts using an
  explicit path allowlist: generated model exports, staging/transport copies,
  temporary Repo stores, scratch build/install trees and other reproducible
  intermediate files that are no longer required. This is the normal experiment
  closeout, not permission for blanket cleanup of `/var/tmp` or old runs of
  uncertain ownership. Preserve source models, shared caches, active workspaces,
  mounted trees and archived evidence. If an artifact is needed to investigate
  a failure, retain only the necessary subset and record its path, size and
  reason in the run report. Record what was archived, removed or retained;
  an external run directory is not permanent evidence storage by default.
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
- Keep the Waf semaphore that compiles source units of at least 128 KiB
  exclusively, including `ServiceUser.cpp` and `ServiceProvider.cpp`;
  smaller units retain two-way parallel compilation. Do not raise the memory
  limit or job count during an editor instability investigation without
  measuring available headroom.
- The safe-build helper may be local-only. If absent, arrange an equivalent
  bounded Waf invocation using available OS resource controls before compiling;
  no particular helper file is mandatory. Do not silently fall back to an
  unrestricted parallel build. Reading or editing source needs no build launcher.

## Git boundary for documentation and specs

- `docs/` and `specs/` are durable local material. Being ignored or untracked
  does not make them disposable: preserve them during cleanup, branch changes
  and experiment resets. Delete their contents only with explicit authorization
  for those paths; never use blanket `git clean` on the checkout.
- The sole documentation tracking exception is
  `docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/`.
  Its paper source, bibliography and figure assets may be tracked. Generated
  manuscript PDFs and compilation intermediates remain ignored.
- Keep paper build tools, measurement code and unused source assets in the
  ignored sibling `NDNSF_Local/`, outside the manuscript source directory.
- Preserve other documentation and Spec ignore rules; do not broaden this
  exception or force-add unrelated files. Keep local Codex state and scratch out
  of commits; this tracked `AGENTS.md` and bundled `skills/` are repository files.
- Put durable implementation changes in source and tests. Keep planning and
  Spec Kit artifacts in the local `specs/` tree.

## Manuscript evidence and revision

- Make the smallest change that explains the mechanism, contribution, method,
  result or necessary limitation. Preserve correct baseline text and organization;
  do not rewrite for style alone. Keep debugging history, task progress and
  internal acceptance instructions out of scientific prose.
- Verify external claims against the original paper's relevant passage, figure
  or experiment; use official primary documentation for software/protocol
  claims when appropriate. A citation marker, abstract-only access or another
  paper's paraphrase does not substantiate details absent from that source.
  Match the actor, mechanism, conditions and scope; distinguish a proposed
  design from implemented or evaluated capability. Record the source location
  in the existing review or response. If unavailable, report the gap and narrow
  or remove unsupported claims; never imply the source was checked.
- Trace implementation claims to source and measured results to original run
  records and analysis. Separate demonstrated results from inference; attribute
  a gain to one mechanism only when the comparison controls other material
  differences. Otherwise describe the combined effect and its limits. Preserve
  negative results and distinguish a baseline's capability from its tested setup.
- For authorized revisions, first check affected prose for concrete actors,
  defined terminology, clear quantities and logical transitions; then review
  necessity, technical accuracy and evidence. A language pass must preserve
  numbers, equations, citation keys, protocol terminology and evidence strength.
  Evidence-backed factual corrections are permitted within the requested scope;
  keep prose, figures, captions, abstract and conclusion consistent. The user's
  requested concision takes precedence over fixed paragraph-count preferences.
  Keep these passes within the existing revision, without a separate report
  process. They do not replace citation, experiment or submission checks.

## NDNSF paper source and compilation

- The NDNSF paper source is maintained in
  `docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework/`.
  Its entry point is `NDNSF.tex`, chapter files are in `sections/`, and the
  bibliography is `sections/references.bib`. Preserve relative figure and
  bibliography paths; do not create a competing source tree or edit PDFs directly.
- Keep this directory self-contained for Overleaf upload, retaining its supplied
  `bibtex/` template files and all active source, bibliography and figure inputs.
  Verify upload packages from a fresh extraction without Base, local tools or
  prior compilation outputs; these must not be required to build the clean paper.
- This manuscript is distinct from the dissertation proposal in
  `docs/PAPER/proposal-defense/`. Do not import claims from another document or
  newer implementation without checking their applicability and evidence.
  Compilation-only requests do not authorize manuscript revisions.
- Use `latexmk` with pdfLaTeX (IEEEtran and BibTeX), running from the source directory. From the repository root:

  ```bash
  cd docs/PAPER/named-data-network-service-framework-paper/Named_Data_Network_Service_Framework
  latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=../.overleaf-build NDNSF.tex
  ```

- Keep auxiliary files in `.overleaf-build/` beside the source directory. After
  successful compilation, inspect `../.overleaf-build/NDNSF.log` for errors and
  unresolved citations/references, and check the PDF with `pdfinfo`.
- From the source directory, publish the verified build by copying
  `../.overleaf-build/NDNSF.pdf` to `../NDNSF.pdf`. Replace only the known generated
  deliverable, never an unrelated/user-supplied PDF or a good output with a
  failed/stale build. Preserve an existing destination of uncertain provenance.

### Fixed baseline and marked comparison

- Keep `docs/PAPER/named-data-network-service-framework-paper/NDNSF_Base/` as the
  fixed, read-only baseline supplied by the user. Do not edit, replace or
  regenerate its sources unless explicitly requested. It remains local-only.
- Use Base as the starting point for necessity review: retain its wording and
  organization wherever they remain accurate and sufficient. In the revised
  manuscript, retain the teacher's abstract from Base, changing only words needed
  to correct a supported factual error or material ambiguity. Do not omit
  necessary mechanism details or evidence limits merely to reduce the diff.
- Make revisions only in the maintained paper source directory, never in the
  baseline or generated comparison TeX.
- Preserve the contribution heading
  `Reusable Identity Credentials with Stable Trust Rules` when that contribution
  is introduced; do not rename it to “One unified Credential.” This heading does
  not waive the need to substantiate security claims.
- After authorized manuscript revisions, build the clean PDF with the direct
  `latexmk` command above and the comparison PDF against the supplied baseline.
  When present, the following convenience script performs both builds:

  ```bash
  bash docs/PAPER/named-data-network-service-framework-paper/NDNSF_Local/tools/build-paper.sh
  ```

- The comparison procedure expands the manuscript's `\input` files and runs
  `latexdiff` against `NDNSF_Base/NDNSF.tex`. Local `make-comparison.py` and
  `diff-preamble.tex` files in `NDNSF_Local/tools/` implement this procedure but
  do not define additional agent rules. Changed tables are preserved as whole old/new tables rather than
  mixing incompatible column layouts. Additions are yellow (RGB 210,165,0 for
  readability on white); deletions are red (RGB 210,0,0). Unchanged text retains
  its normal appearance. Keep the original font/size, with no strikeout,
  underlining, side-by-side columns, or manual markup in the clean manuscript.
- If the convenience files are absent, apply the procedure and formatting above
  with available LaTeX tools. If manuscript sources or the user-supplied baseline
  are absent, report the missing task input; do not invent or fetch a replacement
  baseline. None of these manuscript files is required for unrelated code work.
- Deliver `NDNSF.pdf` (clean paper) and `NDNSF_comparison.pdf` (marked comparison)
  directly in `docs/PAPER/named-data-network-service-framework-paper/`. Keep build
  intermediates in `.overleaf-build/` and `.comparison-build/`, respectively.
  These outputs are local-only and must not be staged.
- Render and inspect changed pages before claiming review completion. Text diff
  markup does not audit changed figure binaries, bibliography database contents
  or preamble/layout changes; inspect and report those separately. An unchanged
  manuscript legitimately produces no colored diff. Never invent edits to show
  markup.

## CodeGraph

When a `.codegraph/` directory exists at the repository root, use CodeGraph
before text search or file reads to locate or understand code:

- **MCP tools** (when available): `codegraph_explore` returns relevant source and
  call paths; `codegraph_node` returns a symbol and its callers or a whole file.
  Load listed deferred tools by name via tool search.
- **Shell** (if installed): `codegraph explore "<symbol names or question>"` and
  `codegraph node <symbol-or-file>` provide the corresponding exploration.
- If indexed tools are unavailable or fail, state the limitation and continue
  with source inspection and `rg`; do not block the task or rebuild the index.
- Without `.codegraph/`, skip CodeGraph. Indexing is the user's decision.

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
