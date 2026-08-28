# Code Experiment Plan

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-23
- Verification Status: UNVERIFIED
- Version Label: code_plan_v1

## Experiment Overview

- **Title**: NDN-SVS RSA Sign/Verify And Ordered Signing Offload
- **Objective**: Measure actual RSA-2048 signing and path-specific verification,
  then isolate the effect of moving publication signing from the Face thread to
  one bounded ordered worker.
- **Hypotheses**: H1 security correctness, H2 observable ordering preservation,
  H3 Face responsiveness improvement, and H4 bounded queue migration.
- **Type**: generic network benchmark

## Source Findings

CodeGraph inspection of exact base `6bb34545` established:

1. `SVSPubSub::publishAsync()` calls
   `prepareAndStageAsyncPublication()`, which calls
   `prepareReservedBytes()` and both RSA sign operations before posting
   `onPreparedPublication()` to the Face `io_context`. Current async publishing
   defers commit but does not offload signing.
2. Fetched outer publication and Mapping Data traverse `Fetcher::onData()` and
   `SecurityOptions.validator`.
3. Encapsulated inner Data traverses
   `SVSPubSub::onSyncData()` and
   `SecurityOptions.encapsulatedDataValidator`.
4. `satisfyPendingFetchFromPiggyData()` currently delivers cached piggyback
   Data directly to subscribers, so a real end-to-end validation claim requires
   common-path repair and negative tests.
5. `KeyChainSigner::sign(Data&)` delegates directly to
   `KeyChain::sign()`. No source contract proves that concurrent callers may
   share one KeyChain/TPM instance, so Spec 136 serializes backend access and
   uses only one formal signing worker.
6. For V2 HMAC Sync Interests, `SVSyncCore::onSyncInterest()` uses a dedicated
   `security::verifySignature` HMAC branch before processing extensions.
   Therefore piggyback mapping is authenticated by the Sync path but is not an
   RSA Mapping Data verification.

## Setup

- **Language/Framework**: C++17, Python 3.8, NDN-SVS, MiniNDN
- **Planned entry command**:
  `sudo -n -E taskset -c 0-3 python3 Experiments/NDN_SVS_RSA_Ordered_Offload_Minindn.py run --manifest build/spec136/campaign-manifest.json`
- **Working Directory**:
  `/home/tianxing/NDN/ndn-service-framework`
- **Dependencies**: Clean NDN-SVS base `6bb34545`, ndn-cxx, NFD, Boost 1.71,
  MiniNDN
- **Environment**: Linux, fixed CPUs, two node namespaces, persistent per-node
  file PIB/TPM

## Variables

| Class | Variable | Levels / control |
|---|---|---|
| Independent | Signing preparation | inline vs one-worker ordered offload |
| Independent | Offered rate | 200, 400, 600, 800, 1000 pps/peer |
| Dependent | Security | validation success/failure and tamper rejection |
| Dependent | Ordering | commit, delivery, callback gaps/reorder |
| Dependent | Responsiveness | attempted/scheduled and release lateness |
| Dependent | Delivery | delivered/committed and end-to-end delay |
| Mechanism | Work location | sign CPU, queue/reorder/commit residence |
| Fixed | Network/protocol | 2 nodes, 10 ms, 100 Mbps, zero loss, V2/HMAC |
| Fixed | Security | RSA-2048, peer fixed trust anchor, real validators |

Potential confounds are subject drift, trust-anchor/certificate retrieval,
KeyChain concurrency, asymmetric peer failure, instrumentation overhead,
release-timer invalidity, and piggyback path differences. The contracts either
freeze or measure each one.

## Expected Outputs

| Output | Path | Format | Success Criterion |
|---|---|---|---|
| Subject manifests | `build/spec136/manifests/` | JSON | Base, patches, builds, linkage, security hashes |
| Receipts | `results/spec136-rsa-sign-verify-offload/<id>/receipts/` | JSON | Exactly ten terminal receipts |
| Peer evidence | `.../cells/` | JSON/JSONL/logs | Two peers and complete security/order accounting |
| Comparisons | `.../rate-contrasts.csv` | CSV | Five inline/offload pairs |
| Report | `specs/136-rsa-sign-verify-offload/evidence/offload-report.md` | Markdown | Bounded hypothesis verdicts and fallacy scan |

## Monitoring Configuration

- **Timeout**: 8 minutes per cell; 95 minutes for the whole formal campaign
- **Monitor files**: terminal receipts, peer stdout/stderr, event streams,
  resource samples, liveness record
- **Experiment type override**: generic
- **Metric file**: peer JSONL plus exact stage summary
- **Metric key**: terminal status, security proof, order proof,
  attempted/scheduled, delivered/committed

## Analysis Plan

- **Primary metric**: p95 Face release lateness with ordering and security gates
- **Secondary metrics**: attempted/scheduled, delivered/committed, offload
  queue/reorder residence, sign/verify CPU, end-to-end p95
- **Success threshold**: SC-007 preregistered `OFFLOAD_EFFECTIVE` rule
- **Comparison**: Paired inline/offload result at each fixed rate
- **Statistical scope**: Descriptive once-only intervention; no inferential
  statistics

## Reproducibility Boundary

No experiment has been run while defining Spec 136. Results remain
`UNVERIFIED` until exact worktree, patch, binary, trust policy, command, route,
and receipt hashes close. Formal failures are retained without retry.
