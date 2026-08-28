# T035 Tiger Candidate 179489

Job `179489` used source manifest
`b532ba5d89c963c31a81dfa2594a42abdc5cfcc5e5a5d3a412807bff46cdaa61`
on three RTX 5000 nodes. NFD startup and inter-node route installation
succeeded, but candidate-local policy construction failed before model
publication or inference.

The retained node-0 error is:

```text
ImportError: cannot import name 'AdaptiveArtifactTransfer'
from 'py_repoclient._py_repoclient'
```

The job terminated `FAILED` after `00:04:12` with exit code `1`; its
`result.json` records the failure and no bootstrap token or selection key was
created.

This is an ABI/runtime closure failure, not a DistributedRepo protocol or
throughput result. The source bundle supplied current Python modules, but the
older SIF still contained a pre-Spec-164 `_py_repoclient` native extension. A
bind-mounted Python tree cannot add symbols to that linked extension.

The replacement gate requires a new Docker image and SIF built from the
current native sources. Both formats must import
`FileSegmentedObjectProducer`,
`fetch_adaptive_segmented_data_packets`, `AdaptiveArtifactTransfer`, and
`ArtifactRepositoryApi` before any Qwen workload is admissible.
