# Research: Data-Centric Authorization Evaluation

## Decision 1: Use data-centric service transaction terminology

**Decision**: Describe NDNSF as a data-centric service framework whose complete
service transaction is represented by producer-scoped Request, ACK, Selection,
and Response Data. Do not use `data-driven` as the primary term.

**Rationale**: `Data-driven` commonly means decisions derived from data analysis.
NDNSF's distinguishing mechanism is that protocol objects are named, signed,
encrypted Data announced through Sync. DNMP already publishes command/reply Data,
and NSC publishes inputs/results as Data, so the contribution is the uniform
general-purpose four-message transaction and its integration with selection and
authorization, not the isolated use of Data.

**Alternatives considered**: “Data-driven service framework” was rejected as
ambiguous. “First fully data-centric service framework” was rejected until a
systematic review can support the word `first`.

## Decision 2: Separate authentication, authorization, and transaction binding

**Decision**: The paper defines three security layers: NDN signatures and trust
rules authenticate the message producer; Controller-managed service permissions
and NAC-ABE attributes authorize service participation and protect content;
one-time UserToken/ProviderToken plus policy epoch bind the current transaction
and reject replay/stale state.

**Rationale**: Decryption proves possession of decryption capability, not signer
identity. Signatures do not alone prove current service authorization. Tokens do
not replace either trust validation or policy authorization.

**Alternatives considered**: “Mutual authentication through encrypted token echo”
was rejected as incomplete and potentially misleading.

## Decision 3: Bound novelty relative to prior work

**Decision**: Claim framework-level integration rather than cryptographic novelty.
Use primary-source comparisons:

- NAC/NAC-ABE already provides named-data confidentiality, automated key
  distribution, and attribute-based access control:
  <https://arxiv.org/abs/1902.09714>.
- MF-IoT uses service attributes for receiver privacy but checks Provider
  eligibility through a service-key trust chain:
  <https://www.winlab.rutgers.edu/~sugangli/papers/mfiot_mag.pdf>.
- DNMP derives command authority from role signing keys and trust-schema rules:
  <https://conferences.sigcomm.org/acm-icn/2019/proceedings/icn19-20.pdf>.
- NSC authenticates its initial notification with a signed Interest, relies on
  out-of-band trust enrollment, and permits NAC for protected input/result Data:
  <https://named-data.net/wp-content/uploads/2021/05/ndn-tr-0074-1-nsc.pdf>.

**Rationale**: These sources show that ABE, Data publication, and trust-schema
authorization each predate NDNSF. The defensible contribution is their particular
composition across a general multi-Provider service transaction.

**Alternatives considered**: A broad claim that other frameworks cannot implement
producer-scoped Data or attribute authorization was rejected.

## Decision 4: State onboarding benefit narrowly

**Decision**: Evaluate and claim absence of per-User Provider ACL/trust-chain
configuration, not absence of all Provider interaction.

**Rationale**: The current policy epoch is the hash of the complete policy file.
A User-only policy change can therefore change the epoch. Providers reject stale
epochs and explicitly fetch current manifest/permission material. This preserves
central policy consistency but means the current implementation may require an
automatic Provider refresh.

**Alternatives considered**: “A new User can immediately use every existing
Provider while Providers remain offline” was rejected as stronger than current
evidence and potentially contradicted by epoch validation.

## Decision 5: Prefer matched same-framework causal comparisons

**Decision**: Measure authorization cost with an isolated, test-only matched
subject that retains the same service transaction, workload, payload, and network
conditions. Treat MF-IoT, DNMP, and NSC primarily as architectural comparisons.

**Rationale**: A cross-framework latency delta would include transport,
synchronization, naming, execution, and retry differences and could not identify
the cost of attribute authorization.

**Alternatives considered**: Reimplementing MF-IoT was rejected as an unrelated
MobilityFirst project. Using gRPC TLS latency as the sole security baseline was
rejected because it tests a different channel-security and endpoint model.

## Decision 6: Register placeholders without implying results

**Decision**: Paper tables use `TBD` plus an explicit planned-experiment note.
No placeholder participates in averages, plots, conclusions, or the abstract.

**Rationale**: Blank or zero-valued cells can be mistaken for missing-at-random or
measured-zero data. `TBD` keeps the evidence boundary visible.

**Alternatives considered**: Estimated numbers and expected-direction arrows were
rejected because they would bias interpretation before measurement.
