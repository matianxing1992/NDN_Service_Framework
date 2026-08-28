# Quickstart: Validate NDN-SVS Experimental Convergence

## Safety Preflight

From `../ndn-svs`:

```bash
git status --short --branch
git show-ref --heads --dereference
git show-ref --verify refs/remotes/origin/master
git remote -v
git reflog show Experimental -n 5
```

Expected: the recorded baseline matches the migration manifest and no remote
write command is part of the workflow.

## Topology Gate

```bash
test "$(git rev-parse master)" = "$(git rev-parse origin/master)"
git merge-base --is-ancestor origin/master Experimental
git show-ref --verify refs/heads/backup/experimental-before-convergence-20260715
git status --porcelain
```

Expected: all commands succeed and status is empty on final Experimental.

## Review Surface

```bash
git log --reverse --oneline origin/master..Experimental
git diff --check origin/master...Experimental
git range-diff origin/master...backup/experimental-before-convergence-20260715 \
                       origin/master...Experimental
```

Expected: only classified mapping, recovery, publication, and build concerns
remain; no WIP/local-workflow commit exists.

## Build And Unit Tests

```bash
./waf configure --with-tests
./waf -j4
./build/unit-tests --log_level=test_suite
```

Expected: Boost 1.71 configures and every test passes.

## Cross-Repository Gate

From `ndn-service-framework` after installing or linking the final NDN-SVS
candidate:

```bash
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation --log_level=test_suite
PYTHONPATH=pythonWrapper python3 tests/python/test_spec112_targeted_timeout.py -v
PYTHONPATH=pythonWrapper python3 tests/python/test_ndnsf_targeted_python_api.py -v
```

Expected: focused non-network tests pass; exclusive MiniNDN cases remain for the
immutable final candidate.

## Final MiniNDN Candidate

Create the candidate with `Experiments/spec112_candidate_manifest.py`, then run
the six cells through `Experiments/NDNSF_Segmented_Response_Minindn.py` using
the exact Spec 112 final configuration. Do not reuse the previous candidate.

Expected aggregate: 6/6 cells successful with the acceptance counts in
[contracts/validation-evidence.md](contracts/validation-evidence.md).
