# T004.m — sender interpreter and transport layout

2026-09-07. T004 remains partial; runtime NOT_RUN.

Source inspection found that `_enter_frozen(..., 'submit')` selected
`runtime.operatorPython` before receiver staging/site checks. The canonical
profile names a Tiger-only environment; a local submit could therefore fail
with FileNotFoundError instead of reaching the existing staging boundary.

Submit now enters its frozen bundle using the invoking host's interpreter and
checks that interpreter's frozen dependency pins. The receiver continues to
verify the configured batch interpreter before sbatch. Batch/run/rank selection
and explicit receiver collect --reconcile retain the cluster path. Profile
digest verification remains before executable selection. No gate was relaxed.

Focused command: `python3 -m pytest -q
Experiments/TigerCluster/tests/test_yolo_operator_env.py
Experiments/TigerCluster/tests/test_yolo_shared_submit.py
Experiments/TigerCluster/tests/test_yolo_submit.py
--junitxml=Experiments/TigerCluster/results/spec183-submit-origin-20260907/focused.xml`.
Result: **68 passed in 14.48 s**. The action matrix includes submit, local,
offline collect, receiver reconciliation, run and rank with a nonexistent
cluster interpreter and mutated-profile rejection. Existing shared receiver
tests retain real files/journal with scheduler doubles. This is component
evidence; no SSH transfer or scheduler/model execution occurred.

Transport decision: preserve absolute paths by creating future qualification
runs in the declared project namespace on both hosts. `_load_prepared` binds
plan.output; `_load_collection_input` consumes original node/reference paths;
the host gate binds sourceSeal.path. Copying bytes to arbitrary new paths cannot
satisfy these contracts. A controlled same-name local project mirror avoids a
new receipt-rewriting or mount-alias mechanism. Local `/project` was absent at
inspection; no directory was created or qualified. Source seals must enter this
layout before new host qualification. Historical evidence is retained unchanged.

The normative transport requirements are in contracts/experiment-profile.md,
Cross-host transport layout. Implement sender closure enumeration, authenticated
file transfer, no-overwrite receiver publication and interrupted-transfer
recovery next. Same-name paths alone do not establish receiver identity. Existing
scontrol ClusterName verification remains necessary. No transport PASS is
claimed; large-SIF read instability remains a separate unresolved blocker.
