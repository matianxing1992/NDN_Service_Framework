# Tasks: NDNSF-DI Three-Role Tk Console

## Phase 0 - Guardrails

- [x] T001 Confirm current dirty worktree ownership before editing
      `NDNSF-DistributedInference/ndnsf_distributed_inference/gui.py` and
      `tests/python/test_ndnsf_di_tk_gui.py`.
- [x] T002 Run `codegraph status .` and, if needed, `codegraph sync .`.
- [x] T003 Preserve existing policy editor, project wizard, model split,
      certificate helper, and old profile loading behavior.

## Phase 1 - Config Data Model

- [x] T004 Add dataclasses for `SharedNdnsfConfig`, `NdnsfSvsEnvConfig`,
      `ControllerTabConfig`, `ProviderTabConfig`, `UserTabConfig`,
      `UserRequestConfig`, and `ThreeRoleGuiProfile`.
- [x] T005 Add profile versioning and migration from existing
      `RuntimeGuiProfile`.
- [x] T006 Add JSON parsing helpers for advanced fields:
      ACK metadata, provider runtime metadata, fragment inventory,
      collaboration roles, key scopes, dependencies, artifact Data names,
      scope-key Data names, role scopes, and NDNSD metadata.
- [x] T007 Add validation helpers for required names, paths, integer fields,
      JSON fields, and service names.
- [x] T008 Add secret redaction helpers for tokens and token-file contents.

## Phase 2 - Role Runtime Controller

- [x] T009 Add a reusable `RoleRuntimeController` state machine with
      `run()`, `stop()`, `restart()`, `status`, `last_error`, and log queue.
- [x] T010 Support direct Python runtime mode using wrapper factories for
      `ServiceController`, `ServiceProvider`, and `ServiceUser`.
- [x] T011 Preserve subprocess mode using the existing command-building pattern
      for app-specific scripts.
- [x] T012 Add environment overlay support that applies selected NDNSF/SVS env
      knobs for a role and restores them after stop when possible.
- [x] T013 Ensure all Tk updates happen through `after()` or a queue, never
      directly from worker threads.
- [x] T014 Add clean application-exit shutdown for all running role controllers.

## Phase 3 - Controller Tab

- [x] T015 Build `ControllerRoleTab` with sections for identity/security,
      policy/permissions, token file, env knobs, and logs.
- [x] T016 Wire fields to `ServiceController(controller_prefix, policy_file,
      trust_schema, bootstrap_identities, serve_certificates,
      bootstrap_token_file)`.
- [x] T017 Add token-file helper UI: open file, create if missing, add/update
      name-token entry, save file, show redacted table.
- [x] T018 Add Validate Policy and Show Effective Config buttons.
- [x] T019 Add Run/Stop/Restart buttons and status label.

## Phase 4 - Provider Tab

- [x] T020 Build `ProviderRoleTab` with sections for identity/security,
      service/roles, ACK metadata, DI runtime, NDNSD/probing, env knobs, and
      logs.
- [x] T021 Wire fields to `ServiceProvider(provider_id, group, controller,
      provider_prefix, trust_schema, handler_threads, ack_threads,
      serve_certificates, bootstrap_token)`.
- [x] T022 Add provider handler modes: echo, static response, NativeTracer
      script fallback, custom command fallback, and dry-run.
- [x] T023 Add ACK handler configuration from status/message/metadata JSON.
- [x] T024 Add NDNSD service-info publishing controls:
      service lifetime and metadata JSON.
- [x] T025 Add provider probing controls if the wrapper/runtime exposes
      `startProviderProbing` or equivalent; otherwise show disabled with a clear
      message.
- [x] T026 Add DI runtime fields for runtime profile, service manifest, native
      plan, fragment inventory, artifact cache, memory/compute profile, and
      deployment id.

## Phase 5 - User Tab

- [x] T027 Build `UserRoleTab` with sections for identity/security,
      permissions/discovery, request settings, payload, collaboration options,
      response, and logs.
- [x] T028 Wire fields to `ServiceUser(group, controller, user, trust_schema,
      permission_wait_ms, handler_threads, ack_threads, adaptive_admission,
      serve_certificates, bootstrap_token)`.
- [x] T029 Add Refresh Permissions using `get_allowed_services()`.
- [x] T030 Add Discover Services using `get_ndnsd_services()` when NDNSD is
      enabled.
- [x] T031 Add payload codecs: text, JSON, hex, and file.
- [x] T032 Add normal request action using `request_service()` in a background
      task.
- [x] T033 Add async request action using `request_service_async()` when the
      user runtime is started.
- [x] T034 Add collaboration request action using `request_collaboration()` or
      `request_collaboration_async()` from JSON roles/dependencies fields.
- [x] T035 Render response status, message, payload preview, elapsed time, and
      errors in the response panel.

## Phase 6 - Main Window Integration

- [x] T036 Replace or reorganize the old deployment runner so the top-level
      operational tabs are exactly `User`, `Provider`, and `Controller`.
- [x] T037 Keep policy/model/certificate tools available as secondary tabs or
      menu entries.
- [x] T038 Add Load Profile and Save Profile for the full three-role profile.
- [x] T039 Add optional Run All and Stop All buttons that delegate to each tab's
      own lifecycle path.
- [x] T040 Add status bar summary for all three roles.

## Phase 7 - Tests

- [x] T041 Extend `tests/python/test_ndnsf_di_tk_gui.py` for new profile
      serialization and migration.
- [x] T042 Add fake runtime factories and test each role starts only after Run.
- [x] T043 Test simultaneous fake Controller/Provider/User lifecycle.
- [x] T044 Test user request dispatch and response rendering with fake
      `ServiceUser`.
- [x] T045 Test JSON-field validation and error rendering.
- [x] T046 Test secret redaction in logs/profile preview.
- [x] T047 Test command/subprocess fallback construction for provider/user app
      scripts.

## Phase 8 - Validation

- [x] T048 Run:
      `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper python3 -m py_compile NDNSF-DistributedInference/ndnsf_distributed_inference/gui.py tests/python/test_ndnsf_di_tk_gui.py`.
- [x] T049 Run:
      `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper python3 tests/python/test_ndnsf_di_tk_gui.py`.
- [x] T050 Run `git diff --check`.
- [x] T051 Run `codegraph sync . && codegraph status .`.
- [x] T052 Optional integration smoke: covered by fake-runtime non-display
      lifecycle/request tests; real `/HELLO` GUI smoke is left for an operator
      session with NFD/MiniNDN running.

## Phase 9 - Documentation

- [x] T053 Update `docs/NDNSF-DI-runtime-workflow.md` with the GUI as an
      operator entrypoint, while keeping CLI commands as reproducible evidence
      paths.
- [x] T054 Add a short GUI profile example under
      `examples/python/NDNSF-DistributedInference/`.
- [x] T055 Document direct API mode versus subprocess fallback and when to use
      each.

## Evidence

- `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache python3 -m py_compile NDNSF-DistributedInference/ndnsf_distributed_inference/gui.py tests/python/test_ndnsf_di_tk_gui.py`: passed.
- `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache python3 tests/python/test_ndnsf_di_tk_gui.py`: 12 tests passed.
- `git diff --check`: passed.
- `codegraph sync . && codegraph status .`: index is up to date.
- A second-pass implementation checklist was reviewed before the final patch was implemented manually.
