# Claim--Evidence Matrix

| ID | Bounded claim | Paper location | Implementation evidence | Prior-work evidence | Required experiment | Status |
|---|---|---|---|---|---|---|
| DC-1 | NDNSF represents a complete service transaction as producer-scoped Request, ACK, Selection, and Response Data announced through Sync. | Abstract; Introduction; Section III-D; Section V-A; Table I | V2 name builders; validator-gated publisher/subscriber path; `results/spec172_publication_audit_20260811_canonical_v2` | NSC and DNMP primary sources | One authorized MiniNDN transaction with four unique Data names, matching producer KeyLocators, SVS producer/sequence evidence, wire digests, and duplicate observations counted separately | SUPPORTED |
| AUTH-1 | NDNSF separates signer authentication from service-semantic authorization. | Introduction; Sections II-C, III-D, and V; Tables I--III | MessageValidator; `/SERVICE` and `/PERMISSION` routing; permission tables; `results/spec172_authorization_smoke`; `results/spec172_authorization_minindn` | NAC/NAC-ABE, MF-IoT, DNMP, and NSC primary sources | 9-case local authorization matrix, composed security regressions, and paired MiniNDN authorized/denied confirmation | SUPPORTED |
| AUTH-2 | One-time UserToken and ProviderToken bind ACK, Selection, and Response to a request and prevent replayed execution. | Introduction; Sections III-D and V; Tables I and III | Token matching and consumed-token sets; `results/spec172_authorization_smoke` | NSC and RICE token context | Token mismatch/replay cases with zero denied executions | SUPPORTED |
| ONB-1 | Adding an authorized User does not require a per-User Provider ACL, identity, or trust-chain configuration change. | Sections III-C, V-A, and VI; Tables II--III | Controller User policy, target-encrypted PermissionResponse, `results/spec172_onboarding`, and `results/spec172_authorization_minindn` | NSC enrollment and MF-IoT Provider trust chain | Three local transitions and paired MiniNDN cases with unchanged Provider-local hashes and zero unauthorized executions | SUPPORTED |
| ONB-2 | The current global epoch requires an unchanged Provider to refresh Controller material when it starts or rejoins after a policy change. | Sections III-C, V-A, and VI; Tables II--III | Whole-policy epoch hash; stale-epoch rejection; explicit startup permission fetch; `results/spec172_onboarding`; `results/spec172_authorization_minindn` | Current implementation boundary | Three stale-then-refreshed local traces plus rejoin-based MiniNDN confirmation; online hot refresh not claimed | SUPPORTED |
| COST-1 | Fresh and cached authorization behavior can be separated into provisioning, ABE key wrap/unwrap, symmetric payload crypto, and receive-key cache operations; the current study does not estimate a full authorization-disabled delta. | Section V; Table III | Runtime crypto counters and timing hooks; `results/spec172_authorization_overhead` | NAC/NAC-ABE cost model | Three 60-s local-NFD cold/warm runs plus nine provisioning scale points | SUPPORTED |

Final audit: all five admitted canonical manifests and every listed artifact
hash validate, and no canonical directory contains an unlisted artifact. The
four-message audit fixes the exact dirty NDN-SVS subject and runtime library,
records four unique validator-approved Data packets, and counts one repeated
observation of the same ACK without attributing its cause or treating it as a
second packet.

## Claim rules

- `PARTIAL` claims may describe implemented architecture but cannot imply measured
  end-to-end correctness, overhead, or operational benefit.
- `SUPPORTED` requires the experiment artifact named in the table, a verified
  manifest, and an exact paper reference.
- `SUPPORTED` may be explicitly scoped to local framework evidence. The paired
  MiniNDN run confirms only its registered authorized and pre-onboarding cases;
  the remaining fine-grained denial gates remain local evidence.
- `REJECTED` claims stay in the matrix with the contradicting evidence; they are
  removed from positive paper prose.
- Paper locations for `PARTIAL` claims describe architecture or a registered
  question only; they do not upgrade pending end-to-end or performance evidence.
