# Gate E job 182387 - source closure failure before Request

- Slurm: `FAILED`, batch exit `1:0`, step exit `5:0`, elapsed `00:03:45`,
  nodes `itiger07-09`.
- Campaign: `spec168-campaign-v3-fa3112471039fabb3f03` (closed; never retry).
- Request ID reserved by the schedule:
  `spec168-fa3112471039fabb3f03-single`.
- Source: `sha256:e090406547cd36ea426b7d6c63784741f60ddb6d2eea3449e7e8e60837686d4a`.
- Source bundle: `sha256:fe70b185eba5384143c0f9ce240aea0e4f4ec8712f01324033e83ddf3a955133`.

All three nodes allocated distinct RTX 5000 GPUs, started NFD, established the
full-mesh faces, and installed routes. Rank 0 then failed while building the
candidate policy because `build-generation-policy.py` dynamically loads
`/source/jobs/prepare-qwen36.py`, which the source bundle omitted. Ranks 1 and
2 timed out waiting for the absent policy-ready event.

No Controller, Repo node, Provider, User Request, ACK, model publication,
artifact fetch, or inference began. This is a deployment source-closure
failure, not a DistributedRepo, CUDA, model, memory, or inference failure.

The local Gate D audit was insufficient because it checked direct shell paths
and imports but did not evaluate `Path(__file__).with_name(...)` dependencies.
The replacement builder now fail-closes on both direct `/source/...` paths and
transitive `with_name(...)` dependencies. It found and includes both
`prepare-qwen36.py` and `register-qwen36-repo.py` before any replacement
campaign is admitted.

Retained SHA-256 values:

- `raw/node-0/policy-build.log`: `a00226a8dff2dd2472f0b734fbd4510b7f84538a1e1bc0420e3e670130301425`
- `raw/terminal-state.txt`: `4f8e9e45f8a9e1843b81eaf3bdf52a6b778d415d23bf985774a9d34a43f69bd5`
