# Quickstart: Validate DistributedRepo Large-Artifact Transport

## Status

This is a validation contract for a planned feature. Commands labeled
`PLANNED` must not be treated as currently implemented or passing.

## 1. Confirm Active Spec and Context

```bash
jq -r .feature_directory .specify/feature.json
test -f specs/164-distributed-repo-large-artifact-transport/plan.md
rg -F "at specs/164-distributed-repo-large-artifact-transport/plan.md" AGENTS.md
```

Expected: all checks identify Spec 164.

## 2. Run Static Contract Checks

```bash
rg -n "NEEDS CLARIFICATION|\\[FEATURE|\\[DATE" \
  specs/164-distributed-repo-large-artifact-transport
git diff --check -- \
  .specify/feature.json \
  AGENTS.md \
  specs/164-distributed-repo-large-artifact-transport
```

Expected: no unresolved placeholders and no whitespace errors.

## 3. Manifest and Security Unit Gate

`PLANNED` focused tests must cover:

- canonical root/page round trip;
- signature/trust success and failure;
- digest corruption at Data, chunk, page, and full-object levels;
- name, size, geometry, algorithm, policy-epoch, and root substitution;
- cycles, excessive depth/entries/bytes, unknown critical fields;
- unsupported algorithm and downgrade rejection;
- cached-byte identity separated from current provenance acceptance.

Expected: every adversarial case fails before activation; no debug bypass.

## 4. Persistence and Recovery Gate

`PLANNED` local integration tests inject a stop after every transition:

```text
enqueue selected task (no ACK-time reservation)
first range
verified chunk
full verification
finalization intent
payload rename
metadata commit
receipt
catalog activation
```

After restart, verify:

- final state is valid non-active or valid active;
- partial data is not discoverable;
- resume never combines identities;
- committed content is not lost;
- GC cannot remove active/leased/finalizing content;
- scalable metadata has no row per 4 KiB Data packet.

## 5. Public API Gate

`PLANNED` minimal programs publish and fetch a deterministic 64 MiB file using:

```python
ref = repo.publish_file(path, name=name, expected_sha256=digest)
repo.fetch_file(ref, destination, verify=True)
```

Repeat with async progress, cancellation, resume, one and three replicas, and
an explicit advanced session. The applications may use only public API fields.

## 6. MiniNDN Smoke Gate

Use one publisher, one repository, and one consumer with a small deterministic
payload. Then add three repositories for receipt/durability behavior. Validate:

- normal NDNSF collaboration selects and enqueues exact store tasks; ACK
  metadata remains advisory and creates no capacity lease;
- bulk Data does not create one NDNSF request per chunk/packet;
- signed root and digest hierarchy verify;
- exact bytes arrive;
- control count remains bounded;
- non-selected/unauthorized repositories cannot commit;
- interruption resumes safely.

MiniNDN is mandatory for final network/security acceptance. Host NFD is only a
temporary diagnostic path and must be cleaned up.

## 7. Matched Performance Preflight

Before formal measurement:

1. establish network ceiling;
2. establish raw segmented NDN ceiling;
3. freeze packet geometry, topology, logging, sampler, and measurement bounds;
4. run 1 MiB and 64 MiB smoke cells;
5. check disk capacity and memory for 1 GiB and 16 GiB cells;
6. freeze admissible cells without examining formal outcomes.

See [contracts/performance-evidence.md](contracts/performance-evidence.md) and
[experiment-plan.md](experiment-plan.md).

## 8. Formal MiniNDN Campaign

`PLANNED` harness:

```text
Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py
```

Run the frozen matrix with one warmup and at least five measured repetitions
per admissible cell. Do not tune thresholds, discard failures, or overwrite a
canonical campaign.

Expected gates:

- digest-only >=85% of matched raw NDN for >=64 MiB;
- signed-manifest within 10% of digest-only;
- no per-packet control/metadata behavior;
- bounded memory and declared amplification;
- zero security/recovery failures.

## 9. TigerCluster Boundary

Do not run TigerCluster or a large Qwen publication campaign until:

- unit and recovery gates pass;
- MiniNDN security gate passes;
- the matched formal campaign closes;
- Spec 164 audit no longer blocks implementation readiness.

TigerCluster then demonstrates external validity and real model-shard reuse; it
does not replace the generic byte-artifact correctness evidence.

## 10. Native Runtime Rebuild Gate

Do not reuse an older NDNSF-DI SIF merely because its Python files can be
overlaid from a source bundle. Spec 164 adds native symbols in both `ndnsf` and
`py_repoclient`; a pure-Python overlay cannot add those symbols to an existing
extension module.

The Repo Python extension has a public compile-time dependency chain:

```text
RepoClient.hpp -> ServiceUser.hpp -> common.hpp
               -> ndn-svs/svspubsub.hpp
```

`NDNSF-DistributedRepo/pythonWrapper/setup.py` must therefore ask `pkg-config`
for both `libndn-cxx` and `libndn-svs`. Never replace this declaration with a
hard-coded `/opt/.../include` path: that only hides the missing dependency in
one image layout. The regression test
`PublicArtifactNativeBuildContractTest` preserves this requirement.

For a Tiger candidate, build
`specs/162-itiger-qwen36-generation/jobs/Dockerfile.spec164-native-overlay`
and require its builder-stage and final-stage probes to import all of:

```text
FileSegmentedObjectProducer
fetch_adaptive_segmented_data_packets
AdaptiveArtifactTransfer
ArtifactRepositoryApi
```

The same probe must import the canonical
`ndnsf_distributed_inference.app_sdk.deployment.APPDeployment` through the
public `ndnsf_distributed_inference.app` module and confirm that its
`from_config()` accepts `state_root`. Symbol-only probing is insufficient:
Job 180020 showed that a current Repo extension can coexist with an older DI
facade yet fail at the first real `ArtifactRepositoryApi.publish_file()` call.

The promotion order is mandatory:

1. run the local dependency-contract test;
2. pass the Docker builder-stage import;
3. pass the final Docker image import;
4. pass the same import as UID/GID `65532:65532`;
5. export the exact image ID and verify archive bytes plus SHA-256;
6. materialize the archive on TigerCluster and pass the same import inside the
   resulting SIF;
7. only then submit an NDNSF-DI/Qwen workload using that SIF SHA-256.

An older generic Qwen runtime probe is necessary but not sufficient: it can
pass while a newly required native symbol is absent. A source bind mount can
replace Python modules but cannot change an already-linked `.so`.

Failure interpretation:

- `cannot import name AdaptiveArtifactTransfer` from an old SIF means the
  native runtime predates Spec 164 and must be rebuilt;
- `fatal error: ndn-svs/svspubsub.hpp` means the Repo extension build lost its
  declared `libndn-svs` dependency;
- a successful Docker build without both import probes is not admissible
  evidence for SIF materialization.
