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
- **Minimum necessary documentation.** Reuse existing task and result records;
  do not create a report, checklist, gate card, review matrix or retrospective
  for each implementation step or retry. Perform the necessary reviews and
  checks directly and record their conclusions once. Keep one concise experiment
  result record with the necessary structured statistics, source/configuration
  identity, measurement boundaries, outcomes and first failure boundary; append
  retries to the same result table. Update interface, behavior and security
  contracts only where they change, defining each fact once and linking to it.
  Before creating a document, identify the actual implementation, user or audit
  decision it supports and why an existing record cannot hold it. Optional skill
  templates do not require additional documents or duplicated tracking fields.
  Documentation must not replace implementation or verification, or delay them
  through administrative prerequisites. Preserve the experiment measurements
  and cleanup receipts required below in the same result record.
- **Default to Chinese with the user.** Reasoning, progress narration and replies
  use Chinese unless the user asks for English or the artifact itself is English
  (source identifiers, code comments, commit messages, papers, slides, English
  documentation sections). Keep paths, commands, identifiers and quoted output
  verbatim. This rule governs communication only; the engineering and
  verification rules below are unchanged.
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
- When you ask the user for a decision or approval and no answer arrives within
  10 minutes, continue with the option you recommended, record the choice and
  its rationale in the active task record, and do not leave the work waiting.
  Prefer the reversible option; stop and ask again only for irreversible or
  destructive actions, and never treat silence as authorization to push,
  delete durable data, or relax a security contract.
- Preserve unrelated worktree and index changes. Stage explicit authorized paths;
  never use `git add -A` or destructive resets. Commit when requested or required
  by the per-task completion rule below. Push only when explicitly requested.
- Keep `tools/dsh-codex-audit/`, `tools/dsh-tps-window/` and the repository's
  `skills/` in Git. Commit changes to plugin source, package manifests, lockfiles,
  tests and repository skill definitions and references with their corresponding
  work. Keep installed copies, `node_modules`, generated package archives and
  machine-specific configuration outside Git.
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
NDNSF-DistributedRepo belongs in C++. Python is a thin host-side interface for calling
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
   **Prepare portable models independently of execution devices.** The default
   ONNX preparation path publishes one reusable graph/weight package and Repo
   handle for compatible CPU and CUDA Providers. Preparation must not require
   knowing the concrete backend before Provider ACKs; bind that backend from
   the selected Provider's admitted capabilities. Switching CPU/CUDA alone must
   not trigger another export, encryption or Repo publication. Keep device- and
   runtime-specific optimized sessions/caches local to the Provider with correct
   cache identities. Separate model variants require demonstrated operator,
   precision or runtime incompatibility, not a device-specific configuration
   default. A generic capability declaration is not proof of compatibility;
   verify actual execution on the supported backends.
   **Container execution is C++ only.** Runtime applications, diagnostics and
   acceptance probes inside runtime containers must use native C++ executables
   and the installed C++ APIs. This includes inference, Controller, Authority,
   Requester, Provider and Repo operations. Do not execute these paths through
   in-container Python scripts, interpreters or bindings, including when a
   binding calls native C++. Keep Python orchestration, configuration generation
   and offline model preparation outside the runtime container. Continue using
   Waf for configure/build/install; build-time tooling is not an exception for
   Python runtime or diagnostic execution.

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
   Gate delivery promotion on the exact artifact: check its required files and
   declared entry points before installation, then execute the actual installed
   entry point in an isolated target environment outside the source checkout
   before deployment. For host control
   wheels, require the console launcher in the wheel and run the installed CLI;
   for SIF delivery, verify the container-native build/SDK/ELF closure and native
   C++ entry points. Preserve failed checks as unqualified and fix the maintained
   packer and a missing-entry regression before retrying. Record exact artifact
   identity and verification scope; source hashes and module imports alone do
   not qualify the delivery.
   For changes to mechanisms shared by CPU and CUDA, first pass focused C++
   checks and a small installed MiniNDN CPU cold/warm run before deploying the
   changed NDNSF applications to Tiger or starting GPU inference. Reuse validated
   local inputs and the shortest meaningful workload. Record cold/warm wall time,
   CPU time when available, source/config identity, pass/fail and the first failure
   boundary in the existing task/evidence record; compare with a matched CPU
   baseline when one exists. Reserve the full matched performance matrix for
   qualification. Before a Tiger Qwen 27B GPU experiment, pass a real Tiger
   Qwen3-0.6B GPU cold/warm run with an independent output/state oracle, actual
   CUDA execution evidence and nonforced cleanup using the same candidate
   application/runtime/SIF. Reuse that result while its frozen candidate remains
   unchanged; changes to the shared code or candidate runtime/SIF require the
   small-model gate again. GPU-specific runtime, device and kernel changes still
   require their corresponding container/device checks.

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
   Before fixing any bug, first inspect the corresponding implementation,
   previous fixes and regression tests on the `KVCache` branch. Pin the inspected
   commit and distinguish an omitted port, an interface-adaptation error and a
   case the branch did not cover. Reuse its established logic and preserve the
   relevant test contracts; do not redesign a solved state machine. If there is
   no corresponding implementation, record the searched scope and then proceed
   from current source evidence. Do not claim an original regression is covered
   by a different test boundary merely because both tests pass.

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
- Plan refactors and related test or experiment repairs as logical batches. Group
  the callers, implementations, registrations, fixtures, launchers and affected
  entry points needed to complete a shared behavior, migration or test harness
  contract. Prefer one task with ordered internal steps for changes that must
  land together. In the existing plan and tasks, identify each batch's owner,
  dependency order, affected build targets and test selectors, and observable
  acceptance criteria. Different selectors or targets may belong to one batch
  when they exercise the same change on the same candidate. Split independent
  outcomes and changes with hard acceptance dependencies that require an
  earlier result. An explicit `STRICT_SERIAL` plan retains its order and exits.
- Finish the related edits and review their combined call path before the batch
  build. Reuse the configured build and run the deduplicated affected targets
  and checks on the same candidate, once per required build profile in the
  initial validation. Run focused diagnostics earlier only when they resolve a
  concrete blocker or fulfill an explicit required gate. Keep each task's
  acceptance and required experiment evidence visible. After a failure,
  preserve the first failing boundary and rerun the affected closure after
  repair, retaining earlier results only where their inputs and prerequisites
  still apply.
- Scope each Spec to one concrete outcome; first check whether qualified evidence
  and maintained code already suffice. For every task, name the remaining gap
  and the smallest change or check that closes it; remove speculative variants
  and duplicate document work. Expand an experiment only when its current
  condition cannot answer the stated question, not to seek a favorable result.
- Storage and Repo changes must be proven locally before any cluster run, with the
  same read API production uses: write an object, then reopen the store in a new
  process or instance (simulating a Repo restart) and read it back by name. An
  in-process round trip is not evidence: it hits the writer's in-memory index and hides
  exactly the restart-time lookup defects that a fresh Repo exposes. A small local
  probe costs minutes; rediscovering the same class of defect through a Slurm
  deployment costs a rebuild plus a queue wait.
- Before using an external library API, read its installed header for the actual
  namespace, signature and version guard. Guessing from memory (`ndn::util::Segmenter`
  instead of `ndn::Segmenter`, a `Name` constructor that does not exist) produces failed
  compiles and misleading retries; one `grep` in `/usr/local/include/ndn-cxx` settles it.
- NDNSF-DI native behavior tests must exercise production C++ targets with C++
  fixtures and assertions. Host-side Python may orchestrate processes or test bindings, but
  cannot replace native behavior or cross-process end-to-end qualification.
- Record actual commands, source/configuration identity, outcomes and first
  failure boundaries. A successful CLI help, import, build or local smoke test
  cannot be reported as successful inference or distributed qualification.
- On failure, distinguish source, build/link, runtime, harness and resource
  boundaries. Revise the affected check before retrying; do not silently change
  model, resource limits, timeout or workload to manufacture a passing result.

### Test and experiment completeness and portability

- Run the host Python deployment suite in a dedicated virtual environment with
  `tests/python/requirements-deployment.txt`. Create it with `python3 -m venv`,
  install the requirements with its `python -m pip install -r`, and run
  `python -m pytest tests/python/test_ndnsf_di_deployment_config.py` with
  `umask 077` using that same environment. The Ops wheel and CLI fixtures use
  the environment's pip for offline builds and installation into an isolated
  consumer. The host distribution's pip is only a bootstrap input.

- Define acceptance by observable behavior, not by a passing command or task
  status. Trace the maintained entry point through its actual producers,
  consumers, serialization, admission and backend execution to the asserted
  result. A helper test or reconstructed pipeline does not cover a different
  production boundary. Cover applicable failure, timeout, cancellation, retry
  and cleanup behavior; do not add unrelated platform/model matrices merely
  for completeness. Concurrent agents share this contract: the coordinator
  verifies delivered evidence before declaring their assigned work complete.
- Put deterministic contract and state-transition checks in unit cases. Use
  integration cases for interactions across actual component, wire,
  persistence or process boundaries. Use experiments for questions that depend
  on deployment, workload or measured performance. Use tiny generated inputs
  and controllable clocks or events where practical. An expected failure must
  assert the specific rejection and any required absence of side effects.
  Initialization failure, fixture absence and zero assertions cannot satisfy
  an expected failure. Record the scope of any fake device or service used.
- Use an independent oracle with explicit coverage and comparison semantics.
  Freeze model/tokenizer revision, quantization, runtime/operator settings,
  prompt/BOS policy, input tokens, generation length, seeds and provider split
  where they affect the result. Record actual values and artifact identities;
  do not merge receipts from different workloads into one acceptance claim.
  For continuous generation, check every required turn and its state/lineage,
  not just the first token or final aggregate. Missing reference tensors,
  shape/length mismatches and incomplete comparisons must fail explicitly.
  Token agreement, internal-state numerical agreement and performance are
  separate claims. Do not loosen tolerances, delete assertions or replace the
  oracle to obtain PASS; any approved criterion change needs a stated rationale
  and new evidence, retaining the original failure and its qualification scope.

- Organize new test and experiment files by behavior and by the entry point that
  consumes them. Reuse the existing owner directory. Check Git visibility for
  each required input and register it with the corresponding build or install
  target when that entry point needs it.
  - Put MiniMUAS application tests, scenario launchers, fixtures, and run results
    in the MiniMUAS repository. NDNSF owns SDK, binding and protocol tests that
    execute without MiniMUAS. Track SDK-facing design and link to MiniMUAS run
    evidence from the relevant NDNSF Spec.
  - Put native unit cases in `tests/unit-tests/`, native integration cases in
    `tests/integration-tests/`, installed SDK consumers in `tests/installed-api/`,
    and MiniNDN regression launchers in `tests/minindn/`. Register native targets
    in the owning `wscript` and map cases to `tests/check-profiles.json` as
    applicable. Embed small, case-specific configuration, schemas and input
    values in the owning test fixture. Put shared read-only schemas, small
    datasets, test configuration and generators in
    `tests/fixtures/<behavior>/`, or in the owning suite's existing `fixtures/`
    directory when only that suite consumes them.
  - Put cross-experiment presets in `Experiments/profiles/`, reusable topologies
    in `Experiments/Topology/`, and model/data preparation recipes and their
    tool version pins in `Experiments/recipes/`. Keep platform-specific inputs
    with the owning platform or adapter, such as `Experiments/TigerCluster/`.
    Keep an experiment's small inputs, topology or templates used only by that
    experiment together under its owning directory. The preset references those
    inputs and the maintained preparation and run entry points. Keep shared
    preparation tools under `Experiments/` and reuse them across presets.
  - Generate per-run configuration, credentials, Repo state and temporary data
    in the explicit work root. Keep downloaded model weights and other large
    assets in the declared cache root and pass their paths explicitly to
    consumers. Keep run results in the selected output location. Track the
    recipe, template or generator that reconstructs the inputs.
- Treat tracked inputs and declared preparation steps as input authorities.
  `results/`, ignored Specs/docs, another checkout, user HOME, temporary files
  and a developer's cache are outputs or caches, never hidden input authorities.
  Move the minimum reusable input or its generator to its owning directory;
  preserve original research evidence in place. A pipeline may consume output
  from an earlier declared step if it can regenerate or verify that output and
  records its producer and identity. Apply the same rule to the independent
  reference and to CI jobs that consume artifacts from earlier jobs.
- Tests own isolated keys, certificates, PIB/TPM, trust anchors and writable
  directories, and release them after workers/callbacks drain. Generate
  test certificates, trust configurations and other writable test artifacts in
  a fixture-owned temporary directory. Pass its paths explicitly, including
  when a test executable is invoked directly from the checkout root. The
  checkout root and the tracked test input directories must stay read-only
  during tests. When changing a fixture's file lifecycle, invoke the affected
  case from the checkout root and check where it writes. Reuse the existing
  native security fixtures, using in-memory KeyChain and scoped
  certificate-file pattern. Embed only test-only key material or generate it in
  the fixture; never import personal/production credentials or enable trust-any
  to make tests run. Resolve read-only fixtures from an explicit source/install
  fixture root, independently of cwd, username, UID and previous runs. Reject an
  invalid explicit root, missing required files and canonical path or symlink
  escapes from the declared root. Keep inputs read-only and give concurrent runs
  separate writable roots and identities. Isolate the working directory,
  HOME, TMPDIR and credential settings for test listing, preflight, execution
  and their children. Drain owned children before removing their state.
- Declare tool/SDK/ABI, package-data, dynamic-plugin, submodule and symlink-target
  dependencies alongside each supported test or experiment entry. Installed
  launchers must consume their declared prefix resources, independently of the
  source checkout. Trace each required input to its producer through actual
  consumers, including dynamic imports, native library/plugin lookup, schema
  includes, registry references and model sidecars. Check the selected package
  and its actual entry point against the install manifest. Declare required
  system libraries, privileges, network, hardware and other facilities per
  profile. Validate required inputs and conflicting options before starting
  actors or changing system state. Report the affected profile, the missing or
  invalid prerequisite and how to supply it.
- On the development host, run real model-backed tests only with the Qwen
  0.6B or 4B checkpoints. Use TigerCluster for larger-model runs and keep
  their original and prepared weights there. Local unit tests can use
  generated tiny fixtures and metadata-only cluster contract checks.
- Large models and datasets may use an external, explicitly selected cache.
  Their versioned download/preparation recipe, source/revision, integrity checks,
  license, tool versions and expected outputs belong in `Experiments/` (or
  `tests/` for test assets). The maintained installer must expose that preparation
  path. Never require a developer's populated cache or download models implicitly
  during a unit test or ordinary installation. Offline preparation must report
  what is missing. Verify the complete prepared asset, including referenced
  weights and tokenizer files where applicable, before marking it ready. Failed
  or interrupted preparation leaves no ready marker. Rebuild the deployment
  from the tracked preset, verified assets, an explicit namespace and a new
  writable root. Generate or provision its own credentials and regenerate
  derived handles, Repo contents and runtime state through maintained entry
  points. Keep deployment credentials separate from test-only fixtures.
- Fresh-clone qualification uses the documented installer and tracked inputs,
  an empty isolated HOME and owned work directories, with developer caches and
  historical results unavailable. Separate the asset-free default test set from
  explicitly selected model/resource tests. Reconcile full suite/case identities
  from native discovery against the tracked profile for the selected build
  configuration. Give each case one execution owner, including cases unique to
  focused targets, and account for conditions that disable cases. Fail an
  unexpectedly empty selection and derive execution counts from actual results.
  Report missing resources for a selected check as unmet prerequisites. Preserve
  genuine wire, authorization, lifecycle and numerical failures. Establish
  fresh source build and installed execution separately. Run the actual installed
  entry outside the checkout with source/build paths unavailable and without
  inherited developer library, package discovery or Python paths. Vary cwd, HOME
  and UID where applicable. When qualifying asset preparation, demonstrate
  success from an empty cache and test rejection of missing or corrupted inputs.
  Preserve original negative evidence.
  Report selected, executed, passed, failed, skipped, prerequisite-blocked
  and unavailable checks separately. A timed-out or interrupted executable
  without final case results leaves unaccounted cases unavailable. Retain its
  first observed assertion failure and last reached case. Preserve the actual
  subprocess exit status through wrappers and log filters. For background work,
  check the owned process or job's final status and receipt even when the
  monitoring command ends. Report opt-in coverage only for selected cases.
  Keep rerunnable commands and compact receipts for verification-only work so
  another maintainer can reproduce the result.
- Name maintained classes, helpers, test suites, targets and launchers by their
  behavior/component rather than a Spec number. Keep Spec IDs as provenance;
  migrate all callers and installed/package entry points together. Published
  wire/schema/digest identities and historical receipts need an explicit
  compatibility migration, not textual search-and-replace.
  Check source-file paths, concrete Waf targets and case selectors separately
  before building a rename batch. For a name-only change, compare the same case
  on both names with the same inputs. A reproduced baseline failure establishes
  parity only for that failure and leaves the check unqualified.

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
  inspection alone.
- For work tracked in `tasks.md`, finish the defined acceptance and commit the
  verified task before starting the next task. For a declared logical batch,
  this rule applies at the batch boundary. Complete its internal steps in
  dependency order, run their shared validation, map evidence to each task's
  exit, and commit the verified batch together before starting the next batch.
  `STRICT_SERIAL` batches that require shared verification belong inside one
  top-level task. Keep failed or untested acceptance open. This is standing
  authorization to commit only the verified unit's changes and tracked
  status/evidence updates, respecting local-only exclusions. Stage explicit
  paths or specific hunks when files contain unrelated pending work.
  For other work, commit only when requested. Push still requires an explicit
  user instruction.
- Keep an active goal running while builds or jobs run, resources are queued,
  or a failed check can still be diagnosed or repaired. Intermediate success
  does not complete the goal. Use blocked only for a repeated actual impasse
  with no meaningful next action; report the concrete condition. If the host
  goal state cannot be resumed through available tools, explain that limitation
  without claiming it was restored, and continue authorized work in the turn.
- Before reporting a DSH context shortage, verify the latest usage and effective
  context limit for the active agent's session and model. Check whether the
  displayed percentage means used or remaining, and account for the next step's
  input, any required output reserve and expected tool results. A generic warning
  or an unverified estimate alone cannot justify a blocked goal. If the reading
  shows more than half remains and there is no confirmed request-size error,
  continue with a bounded step. If the reading is unavailable, stale or conflicts
  with a warning, try a small useful step and check its actual outcome. A
  successful request confirms that step can continue. Recover missing task facts
  from source, the task record or the last verified checkpoint.
  If the host explicitly reports a context limit or verified usage shows the
  next bounded step cannot fit, narrow inputs and tool output and initiate
  supported compaction as needed. Preserve the goal, user constraints, last
  verified checkpoint and next action, then resume with the goal active. Mark
  the goal blocked only when bounded recovery attempts leave no meaningful next
  action. Report the specific step, the observed limit or exact host error, and
  the recovery attempted.
- **Decide rather than stall on a design fork.** When a choice changes already
  written code, state the options with their concrete cost and ask once, in the
  same turn, with a recommendation. If no answer arrives within ten minutes,
  proceed with the recommended option, record it as an assumption in the task
  evidence, and continue; do not idle, keep re-asking, or wait while other
  actionable work exists. A later user answer overrides the assumption and the
  work is corrected then. Reserve genuine blocking for information only the
  user can supply, such as credentials, topology details or an unavailable
  external artifact.
- **Follow the binding contract before extending a design.** A Spec's
  contracts/ and data-model.md may forbid an architecture that looks natural
  from the code alone, for example a second scheduler or a new virtual
  execution port. Read the binding section for the affected mechanism before
  adding a type or port, and treat its prohibitions as requirements: if the
  contract says to extend an existing interface instead, do that even when a
  new interface is easier. If the contract itself seems wrong, raise that
  explicitly instead of diverging silently.

## NDNSF-DI experiment storage

- Portable operating guides live under `Experiments/`, alongside the maintained
  experiment tools: use `Experiments/TigerCluster/README.md` for Tiger/SIF,
  `Experiments/guides/local-validation.md` for installed MiniNDN checks, and
  `Experiments/guides/installation.md` plus `local-development.md` in the same
  directory for installation and builds. Keep operating instructions there
  rather than in ignored root `docs/` or duplicated skill reference bodies.
  Specs, paper projects and original evidence keep their existing owners.
- Tiger login nodes are for light editing, control-input preparation, submission
  and status queries. Run builds, model processing, bulk hashing and inference
  through Slurm. Before each allocation, inspect all nodes, reservations and
  own jobs; choose any eligible node. CPU work does not wait for free GPUs.
  Recheck within the authorized window, normally every 600 seconds, without
  duplicate submissions. Reuse unchanged qualified SIFs, models and matching
  persistent Controller state; verify actual container mounts and account quota
  separately from global disk space and application cache limits.
- Current Qwen3-0.6B MiniNDN execution uses the same installed native Controller,
  ArtifactAuthority, Repo, Provider and Requester applications and deployment
  configuration schema as host/Tiger execution. Use `ndnsf-di cluster up --config
  deployment.json`; MiniNDN requires root and `platform.kind=minindn`, an absolute
  topology path and its `sha256:` digest. Declare every topology host in `nodes`,
  distinct managed NFD sockets and separate credential homes per actor. Keep
  host-side Python limited to topology, configuration and process orchestration. The Qwen
  experiment entry points accept `run --deployment deployment.json --verify`
  to invoke this owner and its C++ first/warm oracle. Explicit historical
  `--profile`/stage arguments retain compatibility experiments; their results
  are not evidence for this shared deployment path.

- Keep diagnostic logging disabled in normal execution; enable it explicitly
  for debugging or a qualification check that requires diagnostic evidence.
  Do not enable verbose or per-token logs implicitly for performance runs.
  Preserve the minimum structured measurements needed for the timing analysis
  separately from diagnostic logging. Report unavailable measurements explicitly;
  disabling logs must not bypass functional, security or lifecycle checks.
- Reuse a stable disk workspace per purpose rather than creating a fresh
  model-sized build/staging directory for every retry. When available, prefer
  `python3 scripts/ndnsf-di-workspace.py --root /tmp/ndnsf-di-workspaces-$(id -u) PURPOSE -- COMMAND {workspace}`;
  it resets only its marked scratch directory under an exclusive lock.
  Commands run inside that directory and receive `TMPDIR` and
  `NDNSF_DI_WORKSPACE`. Supply the explicit root even if an installed helper
  defaults elsewhere. Use absolute command/source paths. A workspace lock
  protects that workspace only. The host build and native install entries
  check free space by filesystem before compilation, then recheck before
  staging and promotion. Count shared build, compiler scratch, staging and
  destination mounts together, with room for concurrent links, replacements,
  backups and a reserve. If capacity is insufficient, report the largest
  consumers and stop. The default reserve is 2 GiB per filesystem and
  `NDNSF_DISK_RESERVE_BYTES` sets an explicit reserve. Use sampled peak growth
  from the same build profile
  to improve future estimates. Record observed peak, retained outputs and
  the reason for retention once in the existing task record for a heavy build.
  Prevent unnecessary outputs before allocating scratch: an audit, source read,
  small edit or focused test does not justify a fresh full build, SDK export,
  checkout copy, installation prefix or model export. Reuse existing qualified
  inputs and build only the affected targets. Use stable purpose names across
  Specs and retries; a new timestamp or Spec number is not a new storage purpose.
  Host NDNSF compilation belongs in the two maintained build trees below;
  `/tmp` is not permission to create a third disposable host build tree.
  Keep host NDNSF installation staging in a stable owned subdirectory of the
  checkout's ordinary `build/` tree. Promote to the selected install prefix and
  retire the stage when promotion is verified. An external private install is
  a time-bounded installed-consumer check with a named owner and retirement
  condition, not a routine build or an extra retained install.
  Necessary isolated dependency/container builds and transport intermediates
  use an owned, reusable purpose directory under `/tmp`, not `/var/tmp`.
  Verify that the actual filesystem is
  disk-backed and has enough capacity; `/tmp` can be RAM-backed on other hosts
  or inside containers. Set `TMPDIR` and explicit build/staging paths accordingly;
  do not assume that `TMPDIR` overrides a tool's configured output directory.
  Keep model caches, original Repo Data, retained experiment workspaces,
  credentials and durable evidence outside automatically cleaned scratch.
  For routine local model work, retain one verified reusable prepared cache
  for the selected Qwen3-0.6B INT4 and YOLO asset identities. Declare its
  location and recipe. Repo publications and per-Provider material and
  assemblies created by a test are run-owned derivatives. Retain them for a
  named live consumer or an explicitly selected warm-reuse baseline. Retain
  compact model, configuration and measurement evidence when retiring a run.
  Historical timing or qualification receipts alone do not require its bulk
  payloads, former install prefixes or regenerated Provider caches.
  Preserve `/var/tmp` itself and existing required contents; do not migrate or
  delete them merely to adopt this convention. Close each task by inventorying
  every output it owns, including failed retries, linked worktrees, private and
  staged prefixes, wheels, model/Repo copies and test environments. Verify the
  promoted artifact and its consumers before retiring the superseded stage and
  redundant install copies. Retain at most one current reusable copy per
  purpose plus any explicitly required comparison baseline. Preserve minimum
  reproducible evidence, then remove idle regenerable outputs through an
  explicit allowlist. Keep required build configuration and active outputs.
  Record what remains, why it remains and when it can be retired. Measure the
  resulting free space on the output filesystem. Resume interrupted closeout
  before adding another heavyweight candidate for the same purpose. Check
  the actual host cleanup policy. Never enable blanket cleanup of active trees.
- Budget large writes by actual filesystem, including bind-mounted paths on the
  same device. Use `findmnt -T` and `df -B1` on the destination or its existing
  parent. Record the owner, reuse key, output roots, additional peak bytes,
  minimum free-space reserve and release condition in the existing task record.
  Include concurrent work and temporary overlap among source/build products,
  install stages, private prefixes, model/Repo data, archives and logs. Count
  shared physical blocks only once. Relocating output within one filesystem
  does not add capacity. Coordinate the budget across agents and worktrees.
  A start check must reject the operation if the remaining space cannot cover
  its estimated additional peak plus the reserve. A warning from an installed
  launcher is a failed admission check. Verify the actual installed entry point
  and its configured output roots.
- For unattended builds, transfers and deployments capable of large growth,
  enforce a finite output budget and the free-space reserve through the existing
  launcher, resource quota or an owned monitor with a defined cancellation path.
  Stop only the operation's owned work and drain its processes if the limit is
  reached. If neither a provable upper bound nor enforcement is available, stop
  at planning until a bounded path is available. After failure or cancellation,
  retain the smallest reproducible evidence and retire the rest of the
  operation's disposable material. Keep identity history as commits,
  configuration, hashes and compact measurements. Moving a whole obsolete
  tree into `results/` on the same filesystem does not reclaim space.
- Bound agent-generated storage as well as experiment outputs. Return targeted
  source ranges and derived summaries instead of entire environments, Waf lock
  environments, recursive dumps or repeated full logs. When delegation is
  authorized, do not fork the full history of a large, long-lived conversation
  into each helper. Start from a fresh scoped handoff with paths, pinned commits
  and necessary facts; select the client's no-history mode when available.
  Full-history forks can copy gigabytes of stored transcripts per helper even
  when each helper performs little new work. This rule does not authorize
  delegation. Keep debug logs bounded and
  disabled after diagnosis. Codex/DSH conversation history, databases and their
  backups remain protected; use client-supported retention or an explicitly
  authorized archive policy, not automatic deletion by age. Agent instructions
  cannot impose a quota on history that the host records automatically.
- Create host control environments from a resolved, stable base interpreter,
  never another disposable environment. Verify interpreter links and `pyvenv.cfg`
  before retiring older environments. Reuse the current validated environment
  for unchanged control code; archive receipts rather than keeping every old
  installed environment or duplicate source/build input.
- Keep model sources, shared content-identity caches and bulky model/runtime
  copies or raw traces in stable artifact storage outside both the source
  checkout and scratch. Keep only the Qwen 0.6B and 4B original checkpoints on
  this development host. Keep the 27B checkpoint and prepared weights on
  TigerCluster, and record their revision, integrity information and preparation
  recipe in the owning experiment. Small generated fixtures follow the test
  portability rules above. Keep compact reports, statistics and manifests in
  `specs/` and `results/`; reference required bulk artifacts by stable location
  and content identity. Git-ignored paths still consume disk space: do not put
  full models, installed runtime libraries or archives of compiled objects there
  merely to retain a run. This policy does not authorize deleting or migrating
  existing evidence without checking its consumers and reproduction needs.
  Keep durable logs/receipts outside scratch. Qwen's default `--fixed-work-root` already
  reuses one locked workspace; explicit `--run-root` is durable evidence.
  YOLO's default result/cache paths are stable; use a fixed `--results-dir`
  for disposable retries and archive required results separately.
  For Slurm containers, configure transient session files and publication spools
  on separately verified allocation-local disk through `storage.scratch_root`;
  do not derive that root from the persistent model/Repo/cache root. Check the
  actual contained `/tmp` and bulk-data mounts, not just their host directory
  names. Small write probes and global free space do not prove that a model-sized
  publication fits the user's quota. Keep durable models, original Repo Data
  and reusable caches outside allocation-private storage.
- Finish each experiment, including failed or interrupted runs, by archiving
  its report, statistics and the measurement data needed to recompute and audit
  its results inside this repository's existing `results/` or active feature's
  `specs/` evidence directory. Retain commands, source/configuration identity,
  outcomes and the first failure boundary. Preserve readable summary tables
  and structured aggregate statistics; after verifying these summaries, delete
  raw experiment data and diagnostic logs unless a necessary subset is still
  needed to investigate an unresolved failure. State which raw data was deleted
  and which analyses the retained summaries support; do not claim that deleted
  per-sample data remains available for recomputation. Include a detailed timing table for every
  experiment and each request/round, including failed or interrupted attempts.
  Separate launch/material preparation, first request and subsequent requests;
  state timing boundaries and denominators, wall time, available CPU time,
  counts and evidence sources. Keep an additive end-to-end partition separate
  from overlapping/nested step measurements; do not label all non-ONNX time or
  wall-minus-CPU as network time. Mark missing measurements and the first
  failure explicitly. Compare matched conditions. Identify the intended change,
  actual work completed and comparable samples on each side. Report failed and
  interrupted samples alongside the comparison, with any exclusions and their
  reasons. Use the measured breakdown to select the next optimization before
  changing code or retrying.
  Store readable tables and structured summary data with the run report.
  Do not copy whole temporary run trees into
  the archive merely to relocate their disk consumption.
- After verifying that the archive is complete and readable and the run's
  processes have stopped, remove that run's disposable artifacts using an
  explicit path allowlist: generated model exports, staging/transport copies,
  temporary Repo stores, scratch build/install trees and other reproducible
  intermediate files that are no longer required. This is the normal experiment
  closeout, not permission for blanket cleanup of `/var/tmp` or old runs of
  uncertain ownership. Retain the latest usable Repo for the current deployment.
  Before retiring an older Repo, archive its experiment summary, configuration,
  model identity and restoration instructions. Check maintained test and
  experiment consumers and migrate them to tracked fixtures or a supported
  preparation path before removing the store. Preserve any exact packets still
  required to reproduce a result until those consumers are resolved. Retain the
  latest usable shared cache for each required purpose after checking active
  deployments. Preserve active workspaces,
  mounted trees and archived summaries. If an artifact is needed to investigate
  a failure, retain only the necessary subset and record its path, size and
  reason in the run report. Record what was archived, removed or retained;
  an external run directory is not permanent evidence storage by default.
- Do not place bulk build objects, install snapshots or model data in
  `/dev/shm`. Its capacity is also needed by VS Code/Chromium. Reuse disk
  workspaces for these operations, including failed retries. Never purge
  another active workspace, mounted trees, retained current models or evidence roots.
- If the local workspace helper is unavailable, use an explicitly owned, stable
  disk scratch directory with exclusive access; keep source/cache/evidence
  outside it. Never replace this constraint with per-retry bulk staging in RAM.

## Host build stability

- Use a compatible local GCC/G++ pair without pinning a particular GCC major
  version. For incremental builds, reuse the compiler paths and versions in
  the configured Waf tree. Keep local build, install and retry steps on the
  same toolchain. If a toolchain change is necessary, reconfigure the same
  maintained tree, rebuild the affected targets and record the new artifact
  identity. Re-baseline measurements compared across the change.
- Use total physical RAM (`MemTotal` from `/proc/meminfo`, with
  1 GB = 1,000,000,000 bytes) to set the default compiler concurrency and
  ceiling.
  - At or below 12 GB, use four jobs by default and cap concurrency at four.
  - Above 12 GB through 16 GB, use six jobs by default and cap concurrency at six.
  - Above 16 GB and below 100 GB, or when memory is unknown, keep the existing
    six-job default and ceiling.
  - At or above 100 GB, default to the available CPU count (`nproc`) with no fixed
    job ceiling. This applies to the OGMA Ubuntu 20.04 VM with 416 GiB RAM
    and 28 vCPUs.
  At or below the six-job ceiling, use supported counts 1, 2, 4 and 6 as
  permitted by the host tier. Explicit job counts must be positive integers.
  Do not impose an artificial compiler memory cap or a desktop-headroom admission gate.
  Do not serialize large source units: Waf's requested job count controls all
  compilation tasks. Lower concurrency only after an observed OOM or at the
  user's explicit request; do not silently restore the old two-job default.
  Keep compiler processes under the invoking user in an isolated resource group.
  Preserve `WAFLOCK` when using an existing configured build directory.
  `scripts/build-safe.sh build` and `scripts/build-safe.sh asan` select the
  ordinary and diagnostic trees. They use the memory-gated default above,
  serialize Waf launchers and perform disk admission without imposing a memory limit.
  Ordinary GCC builds default to `-g1` for source locations. Use
  `--host-debug-info=full` when debugging requires full symbols.
  The invoking launcher supplies the isolated resource group and OOM cleanup.
- Preserve isolated process ownership, build locking and OOM termination so a
  failed compiler does not leave other workers running. Inherit the host's swap
  configuration; do not set MemorySwap/MemorySwapMax or change system settings.
- The helper may be local-only. If absent, pass the applicable memory-tier
  job count to Waf and preserve equivalent process ownership and cleanup.
  Reading or editing source needs no build launcher.
- Install each host dependency once in its maintained system-wide prefix.
  Headers, pkg-config metadata, link inputs and runtime-loaded libraries must
  resolve to the same installed version. Before reusing a dependency, verify
  the selected prefix, source or package identity and required ABI. Use the
  maintained installer to upgrade the existing installation in place. Keep
  build and staging copies outside the active dependency search paths.
  Rebuild affected consumers after an ABI or prefix change and check actual
  library resolution without loader overrides. Retire obsolete installations
  after checking process and consumer references. Keep rollback material
  outside active compiler, pkg-config and loader search paths. Repository
  build trees and sealed container SDKs retain their own declared boundaries.
- Keep exactly two reusable host NDNSF build trees in this checkout: `build/`
  for ordinary builds, release candidates and global installation, and
  `build-ASAN-UBSAN/` for ASAN/UBSAN diagnostics and tests. Reuse one tree per
  mode across Specs and retries; do not switch `build/` into sanitizer mode or
  create per-Spec, timestamped or repeated temporary build/install copies.
  Count build and install outputs across linked worktrees on the shared disk.
  A linked checkout is a source boundary and does not acquire its own routine
  build-tree allowance. When a required isolated candidate cannot use the
  maintained trees, identify the specific verification gap, bound its single
  temporary build/install closure, and retire it at task closeout.
  Each tree must own separate Waf configuration/signature state, object files
  and generated headers; diagnostic executables must resolve the instrumented
  libraries from their own tree. Select the intended tree explicitly through
  maintained Waf configuration, preserving its corresponding `WAFLOCK`; never
  manually edit Waf cache files to switch modes. If configure/output selection
  is broken, repair that path before the affected build instead of bypassing it.
  Only ordinary `build/` artifacts may be globally installed or promoted as
  release candidates; reject global installation from `build-ASAN-UBSAN/`.
  `install_ndnsf_stack.sh` and `scripts/install-global-target.sh` own the
  shared `build/install-stage` under the native install lock. Verify promoted
  files and run supported installed entry points from outside the checkout
  before retiring the stage. On failure, retain the owned stage and first
  error. The next install retires that stage before creating a new one.
  Ordinary unit and integration tests use the existing build products.
  Reserve a private install prefix for checks of the installed contract and
  remove its managed files when those checks finish.
  Preserve both trees' configured state and incrementally build only required
  targets. Do not clean or reconfigure as a routine preflight: require a concrete
  configuration, ABI or stale-output defect before invalidating reusable state.
  Host setup and installation reuse a matching `build/` Waf configuration.
  Reconfigure in place when the requested profile, compiler or Waf configuration
  inputs change, or when explicitly requested. Keep external Waf dependencies
  in stable, per-project SDK cache directories and retire their install stage
  after verification.
  When configuring, pass the intended `--out` explicitly and verify the output
  path recorded by the selected lock before compiling. `--prefix` sets the
  install destination. Build the concrete targets required by the affected
  test profiles and changed cases. Run the relevant `unit` or `integration`
  profile with `python3 tests/run-checks.py --profile PROFILE --build-root build`
  and the Waf-generated roster from that same build. The default delivery build
  may omit test targets. Compile common test sources once when compile flags,
  defines and dependencies match. Preserve instrumentation boundaries and
  existing object identities. Distinguish Waf's
  task counter from actual compiler and linker invocations. When repairing
  incremental behavior, repeat the same build without edits and verify that it
  does not compile or link again.
  An audit or documentation-only change creates no host build tree. Keep
  generated outputs untracked. Preserve the Waf target/output identity for
  display-only changes. When an executable output must move, list the retired
  name, old digest and replacement if any in the change. Use
  `scripts/build-safe.sh run -- python3 scripts/build-storage.py retire-output`
  with the ordinary tree and those exact identities. Check the idle tree,
  configured targets and tracked references before deleting the executable.
  A disk-versus-Waf difference is inventory only. Legacy installer code may
  still default to temporary output or reject in-checkout output; do not invoke
  that legacy path blindly. Use the maintained configured Waf/target-install
  path with explicit `build/`, or repair the installer within the authorized
  task before using it. External dependencies retain their own maintained build
  systems; model, Repo and experiment evidence are separate from compiler output.
- Cluster SIF application-layer builds have their own budget. The container build
  (`adapters/slurm-apptainer/templates/development-runtime.def.in`) must compile with the
  cores the allocation actually gives (`./waf install -j"$(nproc)"`), never a hard-coded
  `-j2`, and the build allocation must request the CPUs and memory that parallelism needs
  (measured: 324 translation units took about 20 minutes at `-j2` with a 2-CPU/16G job;
  request 8 CPU/32G or more when the queue allows). Change only parallelism and allocation
  size, never compiler flags, target lists or output identity.

## Git boundary for documentation and specs

- `docs/` and `specs/` are durable local material. Being ignored or untracked
  does not make them disposable: preserve them during cleanup, branch changes
  and experiment resets. Delete their contents only with explicit authorization
  for those paths; never use blanket `git clean` on the checkout.
- Paper and defense workspaces under `docs/PAPER/` use independent local-only
  Git repositories when requested. Keep all of `docs/` ignored by this main
  repository; do not force-add paper sources, nested repositories or submodule
  gitlinks here. Local document commits do not authorize a remote or a push.
- Keep paper build tools, measurement code and unused source assets in the
  sibling `NDNSF_Local/`, outside the manuscript source directory. They may be
  versioned in the local paper repository; build caches stay ignored there.
- Preserve documentation and Spec ignore rules; do not force-add unrelated
  files. Keep local Codex state and scratch out
  of commits; this tracked `AGENTS.md` and bundled `skills/` are repository files.
- The narrow `Experiments/TigerCluster/docs/*.md` exception permits portable
  experiment manuals in Git. It does not expose root `docs/`, Specs, paper
  repositories, model/cache files, credentials or raw experiment logs.
- Put durable implementation changes in source and tests. Keep planning and
  Spec Kit artifacts in the local `specs/` tree.

## Manuscript evidence and revision

- In the NDNSF-REPO and NDNSF-DI manuscripts, Section III is **Current
  Design** (当前设计) and the following section is **Target Design** (目标设计).
  Current Design must reflect the behavior of the committed `Experimental`
  code. Pin the checked branch commit in each manuscript's README and trace
  affected claims through the production entry points, owners, and consumers.
  Code on other branches, uncommitted changes, and proposed changes belong
  outside Current Design until they are present in the pinned Experimental
  snapshot. Runtime and measurement claims still require their own evidence.
  Target Design describes the intended architecture based on Current Design
  and the agreed changes. Initialize Target Design with a separately editable
  copy of the complete Current Design, including its figures, tables, and
  inline or included subsections. Use distinct labels for its references.
  Each Current Design update must reconcile Target Design against the new baseline
  in the same revision, retaining agreed changes that are still pending and
  incorporating changes now present in Experimental. An edit confined to
  Target Design must leave Current Design and its pinned source basis intact.
  Apply the same rule to both the English and Chinese manuscripts.
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
- For scientific figures and charts, consult the target journal's official
  figure guidelines. When no venue is specified, use a published guide such as
  the Nature Research Figure Guide
  (https://research-figure-guide.nature.com/figures/). Let geometry, visual
  encodings and arrows convey the mechanism or result. Keep labels short and
  put explanations and qualifications in the caption or main text. Use
  accessible colors alongside labels or line styles, preserve editable vector
  sources where appropriate, and proof the rendered figure at publication size
  for legibility, contrast and correspondence with its caption and source.

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
