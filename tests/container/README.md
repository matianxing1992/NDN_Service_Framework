# Spec 108 Container Tests

The default suite is offline and must not contact a registry, Docker daemon,
Slurm controller, iTiger, or a GPU. External-command behavior is exercised
through fixtures and explicit command mocks.

Test layers:

- `contract/`: schema and public CLI/adapter contracts;
- `unit/`: implementation units with no external runtime dependency;
- `integration/`: local Docker/MiniNDN tests, opt-in only;
- `live/`: bounded cloud/iTiger acceptance, opt-in only;
- `fixtures/`: synthetic or redacted measured data with declared provenance.

Fixtures derived from a live system must state whether they are substrate,
candidate, or physical-production evidence. A fixture is never itself a PASS
for a new candidate.

```bash
tests/container/run.sh             # offline tests
NDNSF_CONTAINER_LIVE=1 tests/container/run.sh live
```

The live switch alone is not authorization to submit a job; the named live
test must also receive an explicit profile and unique run ID.
