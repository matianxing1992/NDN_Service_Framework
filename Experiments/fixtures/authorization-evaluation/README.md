# Authorization evaluation input owner

The cases, runtime gate map and manifest schema are maintained inputs to
`Experiments/run_authorization_evaluation.py` and its analyzer. Tests consume
these same files. They contain no credentials or measured results. Runtime
selectors/source hashes are assertions to verify against the selected candidate,
not proof that an evaluation passed.

Recovered from the retained `ndnsf-integration-182` checkout at
`e91ecc91821aa784824e298cb53f28b1f2f30dec`, originally under the ignored
`specs/172-data-centric-authorization-evaluation/contracts/` directory.
The source checkout is provenance only and is not a runtime prerequisite.

Cases and manifest schema retain their original bytes. In `runtime-gates.json`,
the old August hashes remain under `historical_baseline_source_hashes`; they are
not qualification of today's candidate. `subject_source_paths` names the current
portable inputs. Each run hashes those actual files, and analysis verifies the
recorded hashes. Changing code must not silently relabel historical measurements.
