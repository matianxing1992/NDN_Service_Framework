# T035 Tiger Candidate 180020

Job `180020` used the verified Spec 164 SIF
`sha256:537bba98e31876ff2d3a787bb4f1d9f34a85213cd697921c358e124835d64ce7`
and source manifest
`b532ba5d89c963c31a81dfa2594a42abdc5cfcc5e5a5d3a412807bff46cdaa61`
on `itiger07–09`.

The SIF and stage checksums, three NFD instances, inter-node routes, generated
policy, Controller, and three RepoNodes passed. Rank 0 then failed before
artifact publication:

```text
TypeError: APPDeployment.from_config()
got an unexpected keyword argument 'state_root'
```

The current `network_artifact_backend.py` was combined with an older
`ndnsf_distributed_inference.app` facade in the runtime image. The prior local
gate proved that the four Spec 164 symbols existed, but did not prove the
cross-package call contract. This is therefore runtime-package closure
evidence, not a data-plane throughput result.

After rank 0 exited, ranks 1 and 2 remained in their service wait. The
operator sent the job's supported `USR1` signal. Job `180020` terminated
`FAILED 1:0` after `00:08:02`, wrote `result.json`, and removed every bootstrap
token and selection key.

The replacement runtime installs the current DI Python package and verifies
that the public `APPDeployment` resolves to
`app_sdk.deployment.APPDeployment` and that `from_config()` accepts
`state_root`, in Docker builder, final image, non-root image, and SIF probes.
