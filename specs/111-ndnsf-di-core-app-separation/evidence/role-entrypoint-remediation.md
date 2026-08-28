# Controller Ownership and Role-Entrypoint Remediation

Date: 2026-07-14  
Verdict: **PASS for T203-T207 remediation; Spec 111 completion remains on hold**

## Ownership repair

- `APPController.__module__` is
  `ndnsf_distributed_inference.app_sdk.controller`.
- `app_sdk.facades.APPController`, `app_sdk.APPController`, and the deprecated
  root export resolve the same class object; no duplicate implementation
  remains.
- Maintained Python callers under `Experiments/` and `examples/python/` import
  the canonical controller owner.
- The compatibility manifest keeps `APPClient` owned by `app_sdk.facades` and
  records `APPController` as owned by `app_sdk.controller`.
- Optional NDNSF runtime and deployment imports are deferred until real
  controller construction, so the independently installed APP SDK wheel
  remains importable without the network runtime.

## TDD and focused regression

The new tests first failed because `app_sdk.controller` did not define
`APPController` and the campaign runner had no role-import preflight. After the
repair, the focused gate passed 17/17 tests with 0 failures across:

- `test_ndnsf_di_app_sdk_compatibility.py` (4);
- `test_spec111_role_import_preflight.py` (4);
- `test_ndnsf_di_compatibility_manifest.py` (2);
- `test_ndnsf_di_legacy_exports.py` (1);
- `test_ndnsf_di_architecture_imports.py` (5);
- `test_ndnsf_di_installation_profiles.py` (1).

The final combined focused run completed in 15 seconds. A later source-aware
preflight regression increased this file to five tests and the complete DI
suite to 401 tests.

## Real role startup-import preflight

The preflight uses the real LLM pipeline Controller import and Provider/User
entry scripts with `--help`, an isolated source-root `PYTHONPATH`,
`PYTHONNOUSERSITE=1`, and `PYTHONDONTWRITEBYTECODE=1`. It runs before campaign
directory creation and does not instantiate the NDN network runtime.

Candidate result: **PASS 3/3**, `networkRuntimeStarts=0`.

| Role | Return code | Command digest |
|---|---:|---|
| Controller | 0 | `sha256:e8cc568a86ae183c1e19c389d2e42f8184e72e2e85e3f02026a004d034592ad0` |
| Provider | 0 | `sha256:7e1db5853b6fe5a200ad2c45c1e45688084cec33d7553e9b389f76cf40e85096` |
| User | 0 | `sha256:2611da574a67ba3907291c98ab1b5ff5ce79ebcacaf23a80a3e4f74e803dbef2` |

The first baseline probe correctly blocked before output because its separate
Repo extension was absent. The extension was then built in-place inside the
isolated baseline worktree with:

```text
python3 setup.py build_ext --inplace
```

The resulting `_py_repoclient` SHA-256 is
`fa91b75e80392f19c96ec0938ef978ac84f9b140f38e8604b078447506598c3c`.
The preflight now chooses each source root's real Controller import: the
pre-separation root compatibility export for baseline and
`app_sdk.controller` for treatment. Both source trees pass Controller,
Provider and User **3/3**, with `networkRuntimeStarts=0`. The baseline command
digests are Controller
`sha256:f44d09acce7ea02ee87e35fe215a2d6ad8da9e98039c1868f5917f28f847bef5`,
Provider
`sha256:ec16b006d6ab0c891c2ce58e01fc41c369a4366e28c23e0ada426a53bbb3a037`,
and User
`sha256:91c6e640ed568617d896ec7dd9c386c490f4ecb31fbbe102084ad6bdf8df55b0`.
These probes did not start MiniNDN and did not alter or rerun the old 20
campaign cells.

## Scope boundary

This evidence closes T202-T207. The source-aware correction and isolated
baseline extension remove the role-import blocker for a new candidate. The
failed frozen campaign remains immutable and cannot be repaired in place.
