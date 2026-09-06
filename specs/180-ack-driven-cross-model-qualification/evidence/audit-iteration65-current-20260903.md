# Spec180 iteration-65 runner-input audit

Date: 2026-09-03

The source-bound YOLO MiniNDN entrypoint now validates the case policy before
starting NFD, the repository, a Provider, or the user process. The check binds
the Y-A/Y-B/Y-N role set, requires exactly one explicit Provider owner per role,
rejects multi-role and duplicate owners, and requires controller/user/repo/
Provider identities to map to nodes present in the supplied topology. These
checks validate process startup inputs only; the authenticated ACK snapshot
remains the sole runtime placement authority.

Verification:

```text
PYTHONPATH='NDNSF-DistributedInference:pythonWrapper' \
  python3 -m pytest -q \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_spec180_local_gate.py \
  tests/python/test_spec180_inventory.py \
  tests/python/test_spec180_contract_gate.py
36 passed in 2.01s
python3 -m py_compile Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
```

This evidence does not claim a live MiniNDN result. `run_minindn_case` remains
fail-closed until the real NFD/NDN-SVS ACK-to-Response driver is wired, so T011,
T014, and all local/SIF/Tiger qualification tasks remain open.

At iteration 65, the complete Spec180 Python collection was additionally rerun
after the runner change: 121 passed with 19 exporter/runtime warnings. Those warnings
are diagnostic only and do not count as MiniNDN or qualification evidence.
