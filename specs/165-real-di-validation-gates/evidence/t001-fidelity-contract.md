# T001 Fidelity Contract Evidence

Status: PASS.

The versioned record and aggregate validators are implemented in
`Experiments/ndnsf_validation/fidelity.py`; immutable model/workload identity
is implemented in `Experiments/ndnsf_validation/workload.py`. The focused
negative matrix is in `tests/python/test_spec165_validation_fidelity.py` and
`tests/python/test_spec165_workload.py`.

The closure run `results/spec165-local-gates/20260731T052926Z-5debf140`
accepted model digest
`sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a`
and workload digest
`sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9`.
Malformed, contradictory, skipped, stale, cross-run, and lower-tier records
fail closed in the focused tests.
