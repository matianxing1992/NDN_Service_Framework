# Spec186 direct-target audit — 2026-09-14

## Verdict

The specification now states MiniNDN + TigerCluster YOLO as the direct experiment
objective. The executable path and the validation matrix agree with that objective:
candidate closure and exact runtime composition are prerequisites, real MiniNDN YOLO is
first, the same composition then runs on TigerCluster single-node GPU and two-node
normal/negative cases, and an independent two-node reuse run closes the primary path.

## Audit findings and repairs

The implementation routes `submit.py` through `check/prepare/local/submit/collect` and
enforces terminal protocol, numerical, role/GPU, exit and cleanup evidence. The audit
also found two executable defects: Tiger rendering rejected every profile before
dispatch, and YOLO profiles declared YOLOv8n without package/config/key-map inputs. The
runner now maps explicit Y-A/Y-B/Y-N profiles to YOLO26n, and the Tiger job renders an
Apptainer 1.5.3 launch with the same read-only application and case bundles. A second
static pass found that the staged package verifier needed its relative
contracts/catalogue-authority.pub and that portable private-key maps were rejected
by the process builder; both were repaired and Y-B preflight plus process-vector
construction now pass without starting MiniNDN. Candidate loading also enforces that
the repaired source commit descends from the requested 575b43c baseline. The
remaining ambiguity was documentary: static `PASS`, native closure, preflight and Qwen
rows could be read as alternate completion paths. The following documents make the
boundary explicit:

A final static re-audit also found that the local child, SIF and scheduler environments
omitted `/usr/sbin:/sbin`, which can hide MiniNDN's host network tools. All three PATH
vectors now include those directories; this is a launch-environment repair, not runtime
qualification evidence.

- `spec.md` has a direct objective and a four-step primary path; static evidence is
  explicitly a prerequisite and Qwen is conditional.
- `plan.md` identifies the only primary chain as
  `T005 → T006 → T007 → T009 → T010 → T012`.
- `tasks.md` labels the same chain as the direct qualification path and limits T013 to
  evidence reconciliation.
- `validation-matrix.md` marks V06/V07/V09/V10/V11/V13 as direct runtime rows.
- `quickstart.md` starts with the real MiniNDN-to-Tiger execution path, names the Y-A
  atomic profile and the Y-B/Y-N repeats, and states that checks and probes cannot
  substitute for runtime receipts.

## Code and evidence boundary

CodeGraph confirms the maintained entrypoints are the intended path: the MiniNDN runners
own the local process boundary, `submit.py` owns candidate-gated local/Tiger lifecycle,
the Tiger job wraps the replay harness in the declared SIF, and `collect` calls the
terminal validator. No document change promotes the existing
tiny-ONNX collaboration receipt, static pre-dispatch receipts, or historical Spec183
evidence to YOLO qualification.

The direct target is therefore **corrected and unambiguous in the documents**, but it is
not yet achieved. Current direct rows remain `WAITING_EXTERNAL_INPUT` (MiniNDN YOLO) or
`NOT_RUN` (Tiger single/two-node and reuse) until the exact source-sealed base SIF,
canonical YOLO package/config/key-map/model inputs, and retained runtime allocations are
available. The local Y-A/Y-B/Y-N case bundles now have sealed external digests, but the
exact source-sealed base SIF is still building, so no runtime row is promoted.

## Re-audit evidence

- pytest -q Experiments/TigerCluster/tests — 107 passed; the focused candidate suite
  is 45 passed after the source-lineage regression and subtree-root repair.
- python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py
  specs/186-spec184-tiger-qwen-experiments --strict — structural PASS (19 FRs, 8 SCs,
  5 stories, 13 tasks; 5 tasks complete).
- Local Y-B validate_inputs and MiniNdnCaseRuntime.process_specs() pass against the
  staged signed package, catalogue, trust/key maps, topology and case config; this is
  an input/process-vector gate, not a MiniNDN terminal result.
