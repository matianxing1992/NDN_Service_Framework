# Historical contract fixtures

The decision-point inventory, handoff contract and candidate metadata are exact copies from
Git commit `37f8a468`. They are inputs to the existing static reference-contract tests, alongside
the fixture-only process map. Keeping these inputs here lets a fresh clone run those tests
without a private or ignored Spec checkout.

These files describe that historical contract. Their digests, paths, deferred gates and policy
classifications are test data, not a current deployment configuration, SDK inventory, SIF or
experiment result. Runtime applications and deployment preparation must use their maintained
production APIs and explicit admitted inputs, rather than importing these fixtures.
