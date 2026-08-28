# Documentation Semantic Parity

Date: 2026-07-14  
Verdict: **PASS**

English and Chinese NDNSF-DI READMEs each contain exactly one canonical example
for `APPClient`, `APPProvider`, `APPDeployment` from
`ndnsf_distributed_inference.app_sdk` and `APPController` from
`ndnsf_distributed_inference.app_sdk.controller`. Both contain zero deprecated
`from ndnsf_distributed_inference import APP...` examples.

The paired sections agree that:

- APP owns deployment, request and process-local engine orchestration;
- Core validates and executes immutable intents and consistency mechanisms;
- ten Python policy ports are externally injectable while `RunnerAdapter` is a
  distinct execution SPI;
- models/tokenizers are external artifacts;
- Spec 111 validation is MiniNDN-only and creates no OCI/SIF/iTiger claim.

Verification:

- architecture import boundaries: 5/5 PASS;
- APP SDK compatibility and canonical façades: 5/5 PASS;
- operations CLI semantic parity: 1/1 PASS;
- isolated installation profiles: 1/1 PASS;
- direct canonical-example count check: PASS for both languages.
