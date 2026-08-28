# TigerCluster Remote Preflight Evidence

## Preserved failure: job 181097

- Submission: `spec167-repo-preflight-001`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-001`
- Source manifest SHA-256:
  `d46c12c8029d78d801d21268a9b1d46030bef2c20ccb63d7d7b6edf4eb154ba5`
- Nodes: `itiger07`, `itiger08`
- State: `FAILED 127:0` after 1:17
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/preflight/.spec167-repo-preflight-001.partial`

Both ranks passed the current `ndnsf` and Spec 167 import probe. Source and SIF
checksums passed. The rank step then failed before NFD startup because the exact
accepted SIF does not contain `iperf3`:

```text
/source/specs/167-itiger-repo-throughput/jobs/rank-inner.sh: line 45:
iperf3: command not found
```

The failure is retained and is not replaced. Installing a mutable package at
job time is rejected. Source 002 replaces only the external `iperf3` capability
assumption with a checksum-bound Python TCP streaming ceiling/probe and must use
a new submission identity.

## Preserved failure: job 181098

- Submission: `spec167-repo-preflight-002`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-002`
- Source manifest SHA-256:
  `eb2594f409c1f6c8223964d0834d1bf6fe6209532f108b516c67ed3323a986c3`
- Nodes: `itiger07`, `itiger08`
- State: `FAILED 5:0` after 1:15
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/preflight/.spec167-repo-preflight-002.partial`

Both ranks passed source/SIF checksums, application imports, and the Python TCP
ceiling capability probe. NFD then rejected the generated configuration before
opening a socket:

```text
unknown module 'status' under authorize[0]
```

The generic adapter template included a `status` privilege that the pinned NFD
inside the accepted SIF does not recognize; the previously successful Spec
160/161 templates omit it. Source 003 removes that privilege and extends the
local exact-candidate gate to start this rendered NFD configuration and require
it to remain alive until a bounded test timeout.

## Preserved failure: job 181100

- Submission: `spec167-repo-preflight-004`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-004`
- Source manifest SHA-256:
  `d11df3c1681c1495db2331e395b09405e7b1b032829534f4ce4010c451531f28`
- Nodes: `itiger07`, `itiger08`
- State: failed during portable payload preparation after both NFDs and both
  cross-node routes became ready
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/preflight/.spec167-repo-preflight-004.partial`

Both ranks passed installed-runtime imports, Python TCP capability, NFD startup,
TCP face creation, and route installation. Rank 0 then failed before transfer:

```text
FileExistsError: [Errno 17] File exists: '/shared/preflight'
```

The portable job correctly creates the coordination directory before both ranks
use it, while the inherited MiniNDN fixture helper required the whole run
directory not to exist. Source 005 permits an existing clean coordination
directory only when a distinct rank-local data directory is supplied; the
legacy single-directory MiniNDN behavior remains fail-closed. A regression test
uses the exact pre-created directory shape and rejects stale coordination state.

## Unsubmitted incomplete staging identity: source-003

The first `source-003` rsync was interrupted before the small Spec 167 job
directory arrived. A checksum manifest was generated prematurely over the 18
files that were present. This identity is therefore sealed as incomplete and
was never submitted. It is not repaired or reused. Local candidate testing then
proved that the accepted SIF already contains the required installed
`ndnsf`/`py_repoclient` runtime, so source 004 deliberately excludes local
compiled extensions and carries only checksum-bound experiment and job sources.

## Preserved failure: job 181103

- Submission: `spec167-repo-preflight-005`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-005`
- Source manifest SHA-256:
  `f41b28bd1187724d649904a53d4a400f3a69ccbe5e07b9ea01ebb34ae2fd5e1c`
- Nodes: `itiger07`, `itiger08`
- State: `FAILED 9:0` after 2:08
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/preflight/.spec167-repo-preflight-005.partial`

Both NFDs, routes, the 64 MiB forward transfer, repository-side store, and cold
producer reached readiness. Rank 0 then fetched the cold prefix before waiting
for the cold producer's registered-prefix readiness and received:

```text
RuntimeError: Data packet fetch failed for /spec164/repo-cold/preflight:
Nack: 150
```

This is a launch-order race, not a throughput result. Source 006 waits for the
exact `producer-ready` record before the first cold Interest. More importantly,
the exact two-rank shape is now a mandatory local two-container/two-NFD 64 MiB
forward-and-reverse gate, so this class of orchestration defect must pass
locally before another TigerCluster submission.

## Accepted two-node preflight: job 181106

- Submission: `spec167-repo-preflight-006`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-006`
- Source manifest SHA-256:
  `86a5e50db4ea85292633411448f98e9c1886b68ed956d7c6dbbd818c38b8397e`
- Nodes: `itiger07`, `itiger08`
- State: `COMPLETED 0:0` after 3:41
- Promoted evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/preflight/spec167-repo-preflight-006`

Both source and SIF checksums passed. A real 64 MiB cross-node NDN forward
transfer and a fresh-destination reverse transfer reconstructed the full
content digest with zero timeout and retransmission. The forward measured
transfer was 335.06 Mbps and the reverse cold transfer was 312.18 Mbps. These
are preflight observations, not the formal distribution estimate.

## Preserved formal-runner finding: job 181107

- Submission: `spec167-repo-formal-smoke-007`
- Immutable source manifest SHA-256:
  `70d36f8a86dc5cc2e10a5409976def06ffc7b6a14af70a9db26250b654381574`
- State: `COMPLETED 0:0` after 4:49; Slurm step 4:05

The data transfer succeeded, but file timestamps proved that cross-node
runtime barriers implemented as `/project` file polling each incurred roughly
30--60 seconds. `findmnt` identifies `/project` as NFSv4.2, while `/scratch` is
rank-local XFS. The measured 64 MiB transfer itself took only 1.57 seconds.
Consequently, source 007 is accepted as diagnostic evidence but rejected for
the formal repeated campaign: the NFS attribute-cache delay would inflate
campaign wall time without changing transfer goodput.

## Accepted formal-path preflight: job 181110

- Submission: `spec167-repo-formal-smoke-008`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-008`
- Source manifest SHA-256:
  `74a8c1d207efa4969467b2a9133a018062ca47a65479cafff8add020387eec2e`
- Nodes: `itiger07`, `itiger08`
- State: `COMPLETED 0:0` after 0:51; Slurm step 0:08
- Promoted evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/formal-smoke/spec167-repo-formal-smoke-008`

The formal runner now exchanges bounded control metadata directly over the
node data LAN and writes `/project` evidence only after the cell completes.
The 64 MiB forward and reverse transfers passed with zero timeout and
retransmission at 307.40 and 344.12 Mbps respectively. This reduces the same
formal smoke step from 4:05 to 0:08 while preserving the measured NDN data
path and rank-local payload/store/destination paths.

## Preserved formal campaign failure: job 181112

- Campaign/submission: `spec167-tiger-20260731-source009`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-009`
- State: `FAILED 1:0` after 0:07; first warmup retained as `FAIL`
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/campaign/.spec167-tiger-20260731-source009.partial`

Both ranks started NFD, but each made its single TCP face-create request before
the peer listener was ready. Both requests failed with `Error 504` and
`Connection refused`. The raw formal smoke had not forced asymmetric startup,
so scheduler timing happened to hide this race. The replacement gate runs all
four repository subjects and delays one local rank by one second. Face creation
now retries with a bounded two-second attempt timeout and a 30-second overall
budget before any route or measured work starts. The failed campaign identity
and its one failed ledger row remain unchanged and are not reused.

## Preserved replacement campaign failure: job 181115

- Campaign/submission: `spec167-tiger-20260731-source010-r1`
- Immutable source: `/project/tma1/ndnsf-di/jobs/spec167/source-010`
- State: `FAILED 1:0` after 3:36
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec167/campaign/.spec167-tiger-20260731-source010-r1.partial`

The first signed-manifest warmup completed 37 unique-prefix forward and reverse
subtransfers. Forward transfer accumulated 61.47 seconds at 323.16 Mbps;
reverse cold transfer accumulated 63.46 seconds. Timeout and retransmission
counts were zero, and all 37 warm-reuse checks wrote zero duplicate payload
bytes. The outer shell then reached EOF because `srun` inherited the
`schedule.tsv` standard input and consumed the remaining rows. The campaign
therefore retains exactly one PASS ledger row but is incomplete and cannot
support distribution claims. The replacement redirects every `srun` standard
input from `/dev/null`, and the job-contract gate now requires that invariant.
