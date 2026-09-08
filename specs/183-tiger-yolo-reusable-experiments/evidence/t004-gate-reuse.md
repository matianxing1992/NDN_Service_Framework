# T004 typed prerequisite and retained evidence reuse

Date: 2026-09-07. Scope: source and component verification only.

Prepared receipts now use `tiger-yolo-prepared-run-v2`, binding the canonical
inputs/runtime/dispatch identities into the per-run candidate. The reader checks
plan/run/profile/output consistency and rejects old v1 or mutated bindings.
The development provision reader accepts v2; existing v1 artifacts remain historical.

Submission enters the verified frozen CLI and consumes the correct prerequisite
case: local CPU before single GPU, single GPU before two GPU, two GPU before
negative-dependency. It requires matching I/R/E, harness and normalized global
behavior, distinct runs, and a typed normal collector verdict. It recomputes the
retained evidence using the previous prepared plan, rather than loading a mutable
old profile or trusting a generic PASS marker. Normal collection additionally
binds runtime candidate and package/oracle/fixture digests to the saved plan.

Verification command:

```bash
python3 -m pytest Experiments/TigerCluster/tests/test_yolo_gate_reuse.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py -q --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-gate-reuse-20260907/focused.xml
```

Result: **56 passed in 20.60s**. Gate tests freeze actual scripts and read actual
prepared files, but explicitly double the retained collector verdict. They prove
boundary acceptance/rejection and no writes, not native or numerical correctness.
The prepare CLI test now freezes the real CLI, avoiding an empty fixture script
which returned success without doing gate checks. Existing local owner tests had
passed in the first combined invocation; they were not rerun after fixture-only edits.

Initial checks: 48 passed / 3 failed (two outdated READY fixtures and one empty
frozen CLI); new gate tests initially had 16 setup errors from a missing fixture
directory. These were fixture failures, corrected before the passing run above.

Remaining: actual prior runtime evidence, remote path/staging semantics, Slurm
execution owner, then T007 re-audit. `REMOTE_STAGING_NOT_WIRED` and
`RUNNER_NOT_WIRED` remain deliberate failures. No SIF/native/GPU qualification
or cluster job was produced by this change. Retained roots are currently bound
to their preparation paths; remote transport must preserve or explicitly model
that binding rather than rewriting prior receipt contents.

The first post-render profile check rejected `FILE_SIZE_OR_TYPE:validationContract`
after the contract documentation changed. Its declared content identity was stale;
rerendering dispatch after all contract edits is required. This is a content check
failure, not a model-test failure, and does not warrant repeating runtime tests.

Final rerender completed with E
`sha256:b3b064f338ab26c3098b66fe098824cfd5a376a4eced4cd998ed9e9420a9c66d`.
I and R were unchanged. This is a renderer result, not a new qualification PASS.
Source follow-up: renderer writes effective-profile before refreshing profile
file-reference rows. This can retain stale harness/contract identities in the
effective snapshot; the current validator hashes but does not compare that
snapshot semantically. Resolve and verify this ordering before T007 PASS.
