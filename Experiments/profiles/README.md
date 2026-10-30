# Native Qwen3-0.6B experiments

Use the same installed C++ applications on MiniNDN, a host and Tiger: Controller,
ArtifactAuthority, DistributedRepoServer, Provider and Requester. The deployment
JSON uses `schema: ndnsf-di-deployment-v1`. Model, grants, role placement, backend,
resources and conversation turns are configuration inputs. Python manages network
facilities and processes; it does not implement inference or the request protocol.

For MiniNDN, run in the installed control environment with MiniNDN available:

```bash
sudo /path/to/control/bin/ndnsf-di cluster check --config deployment.json
sudo /path/to/control/bin/ndnsf-di cluster up --config deployment.json
```

The Qwen experiment wrapper uses that same owner and can run its native verifier:

```bash
sudo /path/to/control/bin/python Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py run --deployment deployment.json --verify
```

`NDNSF_DI_Qwen06B_Native_Minindn.py` accepts the same arguments. The regression
entry point exposes the same run as `--case qwen06b-native --deployment
deployment.json`. Its success requires the C++ oracle, rather than exit code alone.
`--oracle /path/to/spec190-multiturn-oracle` selects an explicit verifier candidate;
the default is the verifier in the configured installed prefix.

Keep the existing deployment's `release`, `security`, `model`, `requester` and
`artifact_authority` sections. For MiniNDN, set these facility inputs:

```json
{
  "platform": {
    "kind": "minindn",
    "topology": "/path/to/topology.conf",
    "topology_sha256": "sha256:REPLACE_WITH_SHA256SUM"
  }
}
```

Declare every topology host in `nodes`, including forwarding hosts. `node.host`
must match its topology name. Each node needs a managed NFD with a distinct local
UNIX socket. The current facility supports point-to-point MiniNDN topologies.
Assign actors through `actor.node`; use a separate credential HOME for every
actor, even across namespaces, because the filesystem is shared. Provision the
NAC-ABE actors with RSA NDN keys (`ndnsec key-gen -t r IDENTITY`) and the matching
certificates/trust rules. Preserve producer-owned Data prefixes and permissions.
If an EC signing key is also present, native Core prefers it for signing while
retaining RSA encryption. Trust its actual signing certificate and bind the offer
policy to that signing key; the default RSA encryption key alone is insufficient.

The first/warm verifier requires at least two turns, fixed role placement and
actual participating Providers in at least two distinct namespaces. It checks
native outputs, token streams, conversation/state/dependency evidence and complete
process/facility closeout. These are local network namespaces, not separate GPU
hosts. Tiger's distributed-GPU qualification remains a separate check.
Freeze a fresh conversation ID and a valid `retention_deadline_ms` for each new
experiment round. That absolute deadline must cover preparation and the declared
request deadlines; an old frozen configuration can expire during troubleshooting.
The launcher preserves this bound rather than silently extending a grant lifetime.

Successful MiniNDN execution closes its applications, NFDs and topology before
returning. `cluster status` inspects the recorded operation; `cluster down` closes
only its recorded resources after interruption. Unknown ownership stays UNKNOWN.
Diagnostic logs default to off; structured timing statistics remain enabled.

The older `ndnsf-di-qwen06b-local.example.json` and explicit `--profile`/stage-model
commands retain historical preparation and compatibility experiments. The
regression selection `all` remains that legacy suite; it does not include the
new Qwen deployment qualification or establish that old Python apps are native.
