# T224 Persistent Root and Envelope-Key Ownership

**Date**: 2026-07-15  
**Verdict**: PASS  
**Scope**: local contract/unit/static validation only; no NFD, MiniNDN,
container runtime, iTiger or performance matrix was started.

## Implemented boundary

- `RuntimeJournal` no longer creates, reads or persists
  `request-envelope.key` under its state tree.
- The journal consumes an owner-injected `RequestEnvelopeKeyProvider`.
  `StaticRequestEnvelopeKeyProvider` supports direct secret-manager injection;
  `FileRequestEnvelopeKeyProvider` accepts only owner-controlled, owner-only,
  non-symlink raw 32-byte files outside the journal.
- Protected request envelopes use schema v3 and retain only a non-secret
  `keyId`. A bounded keyring writes with the active key, reopens unexpired v1/v2
  envelopes with retained keys and reopens v3 envelopes by exact key identity.
- Missing, malformed, wrong and unavailable keys fail closed. Rotation can
  retain an old key for restart/reopen while new envelopes bind the new key.
- `/tmp`, `/run` and `/dev/shm` state roots are rejected by default.
  `APPClient.from_config()` and `APPDeployment.from_config()` require an
  explicit operator root (argument or `NDNSF_DI_STATE_ROOT`); durable clients
  additionally require an injected provider, `envelope_key_file`, or
  `NDNSF_DI_ENVELOPE_KEY_FILE`.
- MiniNDN and unit paths use the explicit
  `RuntimeJournal.for_test()`/`test_only_allow_ephemeral_state_root=True`
  override. The formal campaign renderer supplies
  `--test-only-allow-ephemeral-app-state`; production paths do not enable it.
- `ndnsf-di` operations CLI requires `--state-root` and exposes
  `--envelope-key-file`; it has no volatile default.

No key bytes are written to journal records, status, evidence or command-line
arguments.

## Focused verification

All commands used
`PYTHONPATH=NDNSF-DistributedInference:pythonWrapper`:

| Suite | Result |
| --- | ---: |
| `tests/python/test_ndnsf_di_runtime_journal.py -v` | 16/16 PASS |
| `tests/python/test_ndnsf_di_request_handle.py -v` | 14/14 PASS |
| `tests/python/test_ndnsf_di_app_sdk_compatibility.py -v` | 10/10 PASS |
| `tests/python/test_ndnsf_di_deployment_workflow.py -v` | 9/9 PASS |
| `tests/python/test_ndnsf_di_ops_cli.py -v` | 1/1 PASS |
| `tests/python/test_spec111_role_import_preflight.py -v` | 15/15 PASS |
| `tests/python/test_spec111_non_regression_analysis.py -v` | 5/5 PASS |

`python3 -m py_compile` also passed for the journal, APP factories, CLI,
MiniNDN user/experiment/campaign launcher and durable-submit microbenchmark.

The mounted-root behavior is covered with a non-volatile owner directory under
the current home; volatile-root rejection is separately asserted. File-provider
tests cover permissions, external location, durable restart, no embedded key
file, wrong key and active-plus-previous rotation.

## Remaining dependency

This closes audit finding P7 and unblocks T225's persistence prerequisite. It
does not authorize T225 while T222-T223 distributed cancellation remains open.
