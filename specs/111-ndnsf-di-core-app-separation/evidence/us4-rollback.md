# US4 Clean Profile Rollback

Verdict: **PASS**

The owner-profile test built ten disjoint wheels (Core, SDK, APP, planner,
three adapters, operations, compatibility aggregate and external optimizer),
found zero RECORD collisions, created a clean virtual environment, installed
all wheels with no source-tree PYTHONPATH, imported every canonical owner, and
then uninstalled every distribution. Importing
`ndnsf_distributed_inference.core` afterward failed as required.

Observed: 1/1 test passed in 12.636 seconds. The compatibility aggregate alone
owns the lazy root `__init__.py` and thin legacy modules; model weights are in
no wheel. Rollback selects/uninstalls the aggregate without mutating external
artifacts, journals, historical evidence, or wire schemas.
