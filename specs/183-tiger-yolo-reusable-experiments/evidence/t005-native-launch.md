# T005 partial: native Provider command wiring

Date: 2026-09-07. Branch: TigerClusterExperiments.

`NodeRuntime.start_provider` now launches the installed di-native-provider through
the shared role-isolated container/process lifecycle. Its arguments follow the
actual `examples/DI_NativeProviderExecutable.cpp` parser, using explicit caller
identities/service/group/controller, one role per process, bounded permission
wait, provisionable offers and role-local `/output/artifact-cache`. Model roles
advertise CUDA on GPU cases, all roles CPU in local-cpu, and Merge always CPU.
It does not advertise preloaded model availability or mount a model package.

Public generated files are expected at `/config/native-execution-plan.json`,
`/config/service-manifest.json`, `/config/trust-schema.conf`; the offer signing
key belongs exclusively to the role HOME as `offer.pem`. This does not put
private material into a frozen script bundle. The preparer/coordinator still
must generate, validate and bind these files and exact policy identities.
The worker rejects missing/symlink inputs, invalid names/budgets and wrong-node
roles before acquiring a lease or launching a process. Existence checks are
not cryptographic validation, permissions, or proof of model readiness.

## Executed evidence

- Initial four launch tests failed on missing `start_provider`, then passed.
- CPU fixture initially omitted the additional roles' PIB files; existing
  isolation validation correctly rejected it. Fixed the fixture, not the gate.
- `python3 -m pytest -q Experiments/TigerCluster/tests --tb=short --junitxml=Experiments/TigerCluster/results/spec183-native-argv-r1/junit.xml`
- Exit 0; **275 passed in 16.18s**, including 15 new application wiring tests.
- Tests observe argv at the subprocess boundary and substitute a short-lived
  Python process. Inputs are explicitly synthetic; no model or SIF executed.

## Remaining scope

T005 remains unchecked. The application coordinator, public/private preparation,
Controller/Repo startup, validated permission/catalogue readiness, persistent
Providers with separate one-shot Users, actual graph/failure regressions and
T006 collector remain required. T004 must later wire every profile option into
these consumers and complete command exposure. No production qualification,
upload or Slurm submission is authorized by this component checkpoint.

Context Mode project health passed, but statistics describe Claude and are not
checkpoint authority. Repository tasks/source/tests remain authoritative.
CodeGraph's filename exploration returned unrelated broad symbols, so the
exact existing renderer and native option parser were inspected directly.
Spec Kit tasks/evidence are updated; this is implementation, not a new ARS
research experiment or a GSD milestone-completion claim.
