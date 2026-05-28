# Generic NDNSF-DistributedRepo Example

This example validates that NDNSF-DistributedRepo is a generic object store, not
an AI artifact API. It stores and fetches three unrelated object types through
the same shared service name `/NDNSF/DistributedRepo`:

- JSON configuration
- telemetry log
- binary blob

Each object is replicated according to its own replication factor. The repo
client uses `store_object(...)` and `fetch_object(...)`; model or artifact
semantics are intentionally absent.

Run in MiniNDN from the repository root:

```bash
sudo -E PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 Experiments/NDNSF_DistributedRepo_Generic_Minindn.py
```

Expected success marker:

```text
GENERIC_DISTRIBUTED_REPO_MININDN_OK
```
