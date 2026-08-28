# Spec 112 Pre-Fix MiniNDN Evidence

**Task**: T012  
**Outcome**: executed-fail, expected and retained  
**Admissible candidate**: `spec112-d2f8bd557936b2fe7925`  
**Identity SHA-256**:
`d2f8bd557936b2fe7925b0328574ed7b9e620b1c23f106de91f6d51a5f95ef96`

## Candidate Selection Corrections

Two earlier identities remain on disk but are not admitted as the T012 matrix:

1. `spec112-7afe8fe71ac36760b8f7` was rejected before output-directory
   reservation or MiniNDN startup. Git global excludes differed between the
   creating user and sudo/root. The candidate tool now disables user-level
   `core.excludesFile` and explicitly excludes private agent configuration.
2. `spec112-a734bdd2134db614c323` ran two diagnostic cells labeled async, but
   runtime logs proved `NDNSF_SVS_ASYNC_PUBLISH ... disabled`. The runtime
   defaults to synchronous publication, so absence of the variable did not
   select async. Those two results are retained but inadmissible for the matrix.

The corrected launcher forces `NDNSF_SVS_ASYNC_PUBLISH=1` for async and `0` for
sync. Unit tests cover both values, and each admissible role log confirms the
declared state. No admissible candidate/cell was rerun or overwritten.

## Exact Commands

All four commands used the same manifest and exact size sequence:

```bash
sudo -n python3 Experiments/NDNSF_Segmented_Response_Minindn.py \
  --candidate-manifest results/spec112-segmented/spec112-d2f8bd557936b2fe7925/candidate-manifest.json \
  --output-dir results/spec112-segmented/spec112-d2f8bd557936b2fe7925/<cell> \
  --mode <normal-or-targeted> \
  [--svs-sync-publish] \
  --sizes '64,4000,5000,6500,8000,16000'
```

The topology was `Experiments/Topology/AI_Lab.conf`, which contains no nonzero
loss setting and is recorded as 0% configured loss. Each isolated role recorded
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1`; all Provider/User logs were
scanned and contained zero large-reference markers.

## Results

| Immutable cell | Publish | Invocation | Exact passes | Failures | Provider alive | Exit | Outer error |
|---|---|---|---:|---:|---|---:|---|
| `results/spec112-segmented/spec112-d2f8bd557936b2fe7925/boundary-async-normal` | async | Normal | 4/6 | 8 KB, 16 KB | no | 2 | 8917 B > 8800 B |
| `results/spec112-segmented/spec112-d2f8bd557936b2fe7925/boundary-async-targeted` | async | Targeted | 4/6 | 8 KB, 16 KB | no | 2 | 8917 B > 8800 B |
| `results/spec112-segmented/spec112-d2f8bd557936b2fe7925/boundary-sync-normal` | sync | Normal | 4/6 | 8 KB, 16 KB | no | 2 | 8917 B > 8800 B |
| `results/spec112-segmented/spec112-d2f8bd557936b2fe7925/boundary-sync-targeted` | sync | Targeted | 4/6 | 8 KB, 16 KB | no | 2 | 8917 B > 8800 B |

In every cell, 64, 4000, 5000, and 6500 bytes reassembled byte-exactly. The
Provider accepted the 8000-byte request, then reported:

```text
Data ... encodes into 8917 octets, exceeding the implementation limit of 8800 octets
```

The diagnostic Provider catches the current exception boundary, emits
`SEGMENTED_PROVIDER_EXIT` with exit code 2, and terminates. Thus the current
checkout does not reproduce the reporter's historical SIGABRT signal, but it
does reproduce the same oversized double-encapsulation root condition, service
loss, 8-KB timeout, and inability to process the subsequent 16-KB request.

Aggregate result: 4 cells, 0 accepted, 16/24 exact responses, 8/24 failures,
zero automatic retries, zero result replacement, and four distinct Provider
epochs.

## Aggregate Identities

| Artifact | SHA-256 |
|---|---|
| `candidate-manifest.json` | `1fbeb6d82901fefa6be4e45dcd4cde70757a659b44de42b4438cbd24964c5dda` |
| `campaign-summary.json` | `a5b35e937ae5bb54f330a4f7dbd41bb61f7a1283f8c6b1c13a9db6567cd80d2d` |
| `campaign-cells.csv` | `e24e6a371fb8f4dbe0c59bd005d70bbd324f207d2957787c71006b5db5079f10` |

Per-cell config, summary, Provider, and User hashes remain in their immutable
directories. The root filesystem remained at 89% use with about 20 GB free.
