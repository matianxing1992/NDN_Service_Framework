# T010 MiniNDN input binding repair

Date: 2026-09-08. Source baseline: c210fb35 plus this checkpoint.
Scope: component input/consumer checks only; T007 N1/N2 remain BLOCK.
No MiniNDN, native build, SIF, model inference or GPU execution occurred.

The existing wrapper now uses the production prepared-candidate, frozen harness,
profile and public preparation verifiers. The caller supplies the retained
issuer preparation digest; the wrapper does not approve a digest computed from
an arbitrary input. Package bytes and protection epoch match that preparation.
Actual per-run offer, recipient and authority private/public key pairs are
checked, along with permissions, identities and public maps, before host output
creation or child launch. No SIF bytes are read by this check.

The wrapper consumes `private/<role>/offer.pem`, replacing the stale global
`.keys/offers` location. Host state, inputs and output are exclusive beneath
`results/<run-id>/host-minindn`. Private maps are mode 0600. Package and epoch
come from the verified profile/preparation. Explicit requester signing and
authority public keys now survive the maintained generic driver's environment
setup; callers omitting these settings retain its existing defaults.

## Retained checks

Artifacts: `Experiments/TigerCluster/results/spec183-minindn-inputs-20260908/`.

| Artifact | Result | Scope |
|---|---|---|
| inputs.xml / inputs.log | 13 passed | Real generated key pairs and prepared files; 11 invalid-input variants fail before any driver invocation; valid launch uses a process double |
| key-consumer.xml / key-consumer.log | 22 passed | Existing driver environment boundary across explicit-key and default cases; stops before network startup |
| key-main.xml / key-main.log | 1 passed | Rerun only the wrapper launch test after adding the two explicit key environment assertions; overlaps inputs.xml |

These checks do not constitute a host qualification receipt. The successful
wrapper marker explicitly reports `qualification: NOT_EVALUATED`.

## Remaining controlling work

- Register normal, permission-denied and exact missing-intermediate-Data cases;
  the wrapper still accepts only Y-B. Historical Y-N ingress/grant cases cannot
  substitute for a post-Selection missing dependency.
- Bound outer execution and prove cleanup of the owned MiniNDN processes and
  network resources. An outer subprocess timeout alone is insufficient.
- Generate N1's semantic host manifest from those same retained executions,
  including protocol, numerical, fault, exit and cleanup records. Current
  hash-only host validation remains insufficient.
- Re-audit T007 before formal T008–T010 execution. Reuse these component results
  unless their producer/consumer binding changes; no historical campaign rerun.
