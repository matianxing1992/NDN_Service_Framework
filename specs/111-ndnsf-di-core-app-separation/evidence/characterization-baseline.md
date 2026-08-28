# Phase 2 Characterization Baseline

Date: 2026-07-14  
Source base: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6` plus the recorded pre-movement Spec 111 test worktree.

## Executed suites

| Scope | Command | Result | Duration/evidence |
|---|---|---|---|
| Compatibility inventory (T008) | `python3 tests/python/test_ndnsf_di_compatibility_manifest.py -v` | PASS, 2/2 | `results/spec111-core-app-separation/characterization-baseline/python-compatibility.log`, SHA-256 `7d75c6dd3e7bdf0107abe54e2e0d307fad01dfedde9399a03858d2e5cb1e0134` |
| Candidate isolation (T010) | `python3 tests/python/test_ndnsf_di_candidate_lineage.py -v` | PASS, 4/4 | `.../python-lineage.log`, SHA-256 `8da95ea1c7b2a60a9ca0e526b915ec143189d809fc31bb74a8c4aed5c00f897d` |
| Architecture/optional imports/legacy exports and CLI/deployment/score golden (T014-T022) | direct `python3 <test> -v` execution of the six Phase 2 files | PASS, 22 passed and 1 skipped | `.../python-phase2.log`, SHA-256 `0f138fe37498454e6b87110fd51d8300133daddb87c94b50f4060bfa3b97d3e2`; Core-only snapshot skip is expected until T042 |
| Native plan/session/runner/cache/attempt/Qwen behavior (T023-T025) | `./build/unit-tests --log_level=message` | PASS, 264/264; optional ONNX fixture branches reported their declared skips | 77 seconds total local characterization; `.../cpp-unit-tests.log`, SHA-256 `87324c4cc9e89f088c9ecd9b5143653c3651a5620f434365f6fd8cb88bfca860` |
| Existing security matrix (T026) | `./examples/run_security_regressions.sh` | PASS, 6/6 unique leaf markers plus aggregate PASS | 71 seconds; `.../security-regressions.log`, SHA-256 `2f7bb5fe157fd37af0bc12b8be8eec24b42970fa09c3b877a1090a081b008ebe` |

The Python total is 28 tests discovered across T008, T010 and T014-T022: 27
passed and one Phase-3 Core-only snapshot was explicitly skipped. The aggregate
C++ run includes the newly named Spec 111 characterizations and all pre-existing
native/security unit cases. No source movement occurred before these results.

`pytest` was unavailable in the system Python, so the repository-native
`unittest` entrypoints were used. An initial `python3 -m unittest
tests.python...` invocation also diagnosed that `tests/` is not a package; it
did not execute or alter the characterized suites. These are infrastructure
diagnostics, not replaced results.

The security aggregate may use a temporary host NFD and is only a migration
characterization. It does not satisfy final Spec 111 network/security
acceptance, which remains MiniNDN-only.

