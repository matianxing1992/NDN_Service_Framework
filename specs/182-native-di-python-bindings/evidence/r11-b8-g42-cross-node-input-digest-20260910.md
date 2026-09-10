# R11-B8-G42 Cross-Node Input Content Digest Evidence

## Boundary

`run-allocation-topology.sh` previously treated per-node `workdir` and
`identityRef` visibility as sufficient.  A real allocation can expose the
same path at different revisions on different nodes, while MiniNDN normally
uses one filesystem.  That split can produce different model/configuration
inputs or different signing identities after all startup markers have passed.

## Correction

- `allocation_topology.directory_digest` defines a deterministic digest over
  relative directories, relative regular-file paths, and file bytes.
- The supervisor computes the expected workdir digest before startup and
  compares it on every target node.
- Each non-NFD identity root receives the same submit-side and target-node
  comparison after PIB/TPM visibility and symlink checks.
- Missing, special, symbolic-link, or mismatched entries fail before NFD
  startup.  Expected values are retained in `workdir-digest.txt` and
  `identity-digests.tsv`.

## Verification

| Gate | Result |
| --- | --- |
| `python3 -m pytest -q tests/container/itiger-qwen-live/unit/test_allocation_topology.py` | 29 passed |
| `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh` | `NETWORK_SCRIPT_PASS`; injected digest mismatch exits 4 with `survivors: 0` and no NFD log |
| `bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh` | PASS |
| `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py` | PASS |
| `git diff --check` | PASS |

The fake `srun` integration exercises the pre-start mismatch boundary and
does not qualify real Slurm, shared storage, SIF, GPU, cross-node NDN, or
no-Python execution.  Those remain T014--T017/R11-B9 obligations.
