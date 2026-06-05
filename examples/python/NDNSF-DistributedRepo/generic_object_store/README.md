# Generic NDNSF-DistributedRepo Example

This example validates that NDNSF-DistributedRepo is a generic object store, not
an AI artifact API. It stores and fetches three unrelated object types through
the same shared service name `/NDNSF/DistributedRepo`:

- JSON configuration
- telemetry log
- binary blob

Each object is replicated according to its own replication factor. The client
uses the high-level generic object API. In a running NDNSF deployment, repo
nodes preload the deployment config as a normal repo object. The client only
needs the repo service bootstrap parameters and first fetches that config from
the repo through the same NDNSF API:

```python
repo = DistributedRepo.from_repo_config(
    controller="/example/repo/controller",
    user="/example/repo/user",
    group="/example/repo/group",
    trust_schema="examples/trust-schema.conf",
    config_object_name="/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml",
)
manifest = repo.put("APP/Generic/BinaryBlob/demo", payload,
                    object_type="binary-blob", replication_factor=3)
payload = repo.get(manifest.object_name, manifest)
```

`repo.put(...)` accepts an application-relative suffix and publishes it under
the user's namespace, for example
`/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo`.
The repo stores the app-created, signed Data segments for that object. It does
not add another encryption layer; encrypted and plaintext app payloads are both
treated as opaque bytes before segmentation and signing.
Trust schemas should validate these Data packets hierarchically: the Data name
must be under the publisher identity, child certificates must remain under
their parent certificate namespace, and production deployments should anchor
validation at the trust-root certificate.

The config object is created by the deployment side when `repo_node.py` starts.
It is also a normal repo object, so its object name is under the controller
publisher namespace. It is not something the application user hand-writes
before calling the API.
For local tests and offline tooling, `DistributedRepo.from_config("repo_policy.yaml")`
is still available.

The example intentionally avoids model or artifact semantics.

## Persistent Repo Catalog Gossip

The MiniNDN smoke starts three Persistent repo nodes and a small
`catalog_sync.py` sidecar beside each repo. The sidecar periodically asks peer
Persistent repos for `CATALOG_DELTA` and merges the returned entries into its
local repo with `CATALOG_MERGE`.

The smoke intentionally uses a 10 second sync interval. A shorter all-to-all
interval can create too many NDNSF service requests and delay normal repo
operations, which is exactly why catalog exchange should stay object-level and
delta-based rather than per-segment or full-directory broadcast.

After storing JSON config, telemetry log, binary blob, and app-signed Data
packet objects, the client waits for catalog propagation and asks every
Persistent repo for `CATALOG_SNAPSHOT`. The smoke succeeds only when every
snapshot contains every stored object.

## Repair Plan and Manual Repair Action

Catalog lookup and snapshot responses expose control-plane health for each
object. When an object has fewer live replicas than its configured
`minReplicationFactor`, the catalog marks it as under-replicated and includes a
`repairPlan`. The plan lists conservative candidate actions with:

- the object name and object hash;
- the live source Persistent repo;
- the target Persistent repo;
- the configured min/max replication factors.

By default, the sidecar does not execute those actions. It only prints a warning
when a repair action targets its local repo. This keeps catalog synchronization
safe for deployment testing and avoids silent background copying.

To allow a sidecar to execute repair actions that target its own repo, set:

```yaml
repo_control_plane:
  repair:
    auto_execute: true
```

or start `catalog_sync.py` with `--auto-repair`. Use `--no-auto-repair` to
force warning-only behavior even when the config enables repair.

Repair execution is orchestrated by the client/sidecar path rather than by a
provider recursively calling another repo provider. The sidecar prepares the
source object with `FETCH_PREPARE`, verifies the object hash, publishes a packet
manifest, and asks the target repo to ingest the signed Data packets with
`STORE_PACKET_PULL`. The target repo stores opaque signed Data packets and
updates its catalog entry; it does not decrypt or reinterpret the object.

The MiniNDN health smoke checks this path with:

```text
GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK
GENERIC_DISTRIBUTED_REPO_CATALOG_HEALTH_OK
```

## Namespace Design

Application data is named by the publisher, not by the repo service. The repo
service name stays shared and stable:

```text
/NDNSF/DistributedRepo
```

Object names are globally unique because the high-level API expands a relative
application suffix under the publisher identity:

```text
repo.put("APP/Generic/BinaryBlob/demo", payload)
  -> /example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/APP/Generic/BinaryBlob/demo
```

The deployment config object follows the same rule. It is published under the
controller identity:

```text
/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml
```

Payload segments are Data packets under the original publisher namespace, for
example:

```text
/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/<digest>
```

The repo does not rename the data into `/NDNSF/DistributedRepo` and does not
wrap it in another encryption layer. If the application already encrypted the
payload, the repo stores encrypted bytes; if the application left it plaintext,
the repo stores plaintext bytes. In both cases the repo client segments and
signs the Data packets, then repo nodes store those segments.

## Trust Schema Design

The MiniNDN deployment uses a project root identity:

```text
/example/repo
```

It signs child identities such as:

```text
/example/repo/controller
/example/repo/user
/example/repo/provider/repoA
```

This satisfies hierarchical parent-child trust because every issued certificate
remains under the project root namespace. A root identity such as
`/example/repo/root` signing `/example/repo/user` would not be a parent of the
user namespace, so the example does not use that pattern.

The generated trust schema follows these rules:

- stored object Data must be under the publisher identity;
- NDNSF runtime Data and SVS sync Data must be under the signer identity;
- child certificates must be under their parent certificate namespace;
- production deployments anchor validation at the project trust-root
  certificate, not at `type any`.

Run in MiniNDN from the repository root:

```bash
sudo -E PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py
```

Expected success marker:

```text
GENERIC_DISTRIBUTED_REPO_CATALOG_GOSSIP_OK
GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK
GENERIC_DISTRIBUTED_REPO_OK
GENERIC_DISTRIBUTED_REPO_MININDN_OK
```
