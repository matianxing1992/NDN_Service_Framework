# NDNSF Project Documentation

This directory contains the active engineering documentation for NDNSF. Paper,
proposal, slide, and dissertation material is archived under
[`docs/PAPER/`](PAPER/README.md) so day-to-day project docs stay focused on the
codebase.

## Start Here

- [Architecture](architecture.md): framework layers, runtime flow, and major
  module boundaries.
- [Module Map](module-map.md): where the core, Python wrapper, DistributedRepo,
  DistributedInference, UAV app, examples, and experiments live.
- [Build And Test](build-and-test.md): install/build commands and validation
  suites.
- [Experiments](experiments.md): MiniNDN-first experiment workflow and evidence
  conventions.
- [Security Model](security-model.md): permissions, NAC-ABE routing, tokens,
  Targeted invocation, and local invocation boundaries.
- [Streaming Substrate](streaming-substrate.md): app-neutral C++ stream
  session/chunk/FEC helpers and the UAV video mapping boundary.
- [Core/App Boundary](ndnsf-core-app-boundary.md): which reusable mechanisms
  belong in NDNSF core and which semantics stay in Repo, UAV, and DI.
- [Native DI Roadmap](native-di-roadmap.md): current NDNSF-DI native execution
  status and next gates.
- [Documentation Policy](documentation-policy.md): where to put docs, papers,
  generated outputs, and result artifacts.
- [Development Branching Policy](DEVELOPMENT_BRANCHING_POLICY.md): temporary
  Spec branch lifecycle and the required `Experimental` → `main` merge path.
- [Engineering Reports](reports/README.md): dated validation reports and
  supporting evidence that should not replace current project docs.

## Current Engineering Direction

NDNSF is a generic dynamic service framework over Named Data Networking. The
supported direction is the runtime API, not generated service/stub classes:

```cpp
provider.addHandler<RequestT, ResponseT>(serviceName, handler);
user.RequestService<RequestT, ResponseT>(
  serviceName, request, ackMs, policy, timeoutMs, onResponse, onTimeout);
```

The active validation workloads are:

- Core NDNSF service invocation and security regressions.
- DistributedRepo for large artifact and object storage.
- NDNSF-DistributedInference for native multi-provider dataflow.
- UAV as an application-driven network workload.

MiniNDN is the default validation surface for network/security/performance
work until an experiment explicitly requires real hardware.

The accepted post-Spec-084 boundary uses only V2 invocation, typed capability
and operation envelopes, provider-owned fail-closed leases, C++ Core stream
state, and one canonical Repo network adapter. Application scheduling, storage,
mission, codec, and model policy remain outside Core.

## Paper Archive

Use `docs/PAPER/` for proposal sources, survey PDFs, API-paper LaTeX, slides,
and reference PDFs. Do not put active implementation guidance inside paper
directories; summarize durable engineering decisions in this `docs/` root
instead.

## Reports

Use `docs/reports/` for historical audits, validation notes, and dated
experiment summaries. If a report changes the current design or workflow,
promote that decision into the active docs above.
