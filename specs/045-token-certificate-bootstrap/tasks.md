# Tasks: Token Certificate Bootstrap

## Phase 1: Setup

- [x] T001 Set `.specify/feature.json` to `specs/045-token-certificate-bootstrap`
- [x] T002 Update the active Spec Kit pointer to `specs/045-token-certificate-bootstrap/plan.md`

## Phase 2: Foundational

- [x] T003 Add CertificateBootstrap TLV helper declarations in `ndn-service-framework/CertificateBootstrap.hpp`
- [x] T004 Implement CertificateBootstrap TLV encode/decode and client helper in `ndn-service-framework/CertificateBootstrap.cpp`
- [x] T005 Add ServiceController token table and endpoint declarations in `ndn-service-framework/ServiceController.hpp`

## Phase 3: User Story 1 - Token-Based Controller Signing

- [x] T006 [US1] Implement ServiceController token file loading and certificate issuance in `ndn-service-framework/ServiceController.cpp`
- [x] T007 [US1] Register `/NDNSF/CERTBOOTSTRAP` handler in `ndn-service-framework/ServiceController.cpp`
- [x] T008 [US1] Add `--bootstrap-token-file` to `examples/App_ServiceController.cpp`
- [x] T009 [US1] Add `--bootstrap-token` client flow to `examples/App_User.cpp`
- [x] T010 [US1] Add `--bootstrap-token` client flow to `examples/App_Provider.cpp`
- [x] T011 [US1] Add sample token file `examples/hello.bootstrap-tokens`

## Phase 4: User Story 2 - Manual Flow Compatibility

- [x] T012 [US2] Preserve no-token behavior in user/provider startup and existing regression scripts
- [x] T013 [US2] Add wrong-token diagnostic path to the token bootstrap regression

## Phase 5: User Story 3 - NDNCERT Token Challenge

- [x] T014 [US3] Add token challenge header in `/home/tianxing/NDN/ndncert/src/challenge/challenge-token.hpp`
- [x] T015 [US3] Add token challenge implementation in `/home/tianxing/NDN/ndncert/src/challenge/challenge-token.cpp`
- [x] T016 [US3] Wire token challenge into the ndncert build if source globs do not pick it up automatically

## Phase 6: Validation

- [x] T017 Add `examples/run_token_certificate_bootstrap_regression.sh`
- [x] T018 Build NDNSF examples and tests
- [x] T019 Run `examples/run_hello_auth_regression.sh`
- [x] T020 Run `examples/run_token_certificate_bootstrap_regression.sh`
- [x] T021 Run MiniNDN HELLO/token bootstrap validation
- [x] T022 Build or smoke-test ndncert token challenge

## Phase 7: Reuse and NDNCERT Format Alignment

- [x] T023 Add local controller-signed certificate detection before token bootstrap
- [x] T024 Switch App_User and App_Provider from forced request to ensure/reuse behavior
- [x] T025 Document the shared `<identity> <token> [role]` token-file format
- [x] T026 Extend shell regression to verify repeat startup reuses the local certificate
- [x] T027 Extend MiniNDN validation to verify repeat startup reuse and single controller issuance

## Phase 8: Runtime API and Config Surface

- [x] T028 Add controller/user/provider bootstrap parameters to the pybind native API
- [x] T029 Expose `bootstrap_token_file` and `bootstrap_token` in the Python object API
- [x] T030 Expose bootstrap token settings in process orchestration config dataclasses
- [x] T031 Add Python API/config tests for generated bootstrap flags
- [x] T032 Document the Python API/config bootstrap usage in quickstart
- [x] T033 Build the pybind11 Python extension with the new native constructor signatures
- [x] T034 Run Python API/config tests and C++ token-bootstrap regression after API/config changes

## Phase 9: Direct Python End-to-End Smoke

- [x] T035 Add a direct Python ServiceController/ServiceProvider/ServiceUser token-bootstrap smoke driver
- [x] T036 Add a shell regression that isolates `HOME`, runs the direct Python smoke, and verifies certificate reuse
- [x] T037 Run the direct Python token-bootstrap regression
- [x] T038 Document the direct Python bootstrap smoke command in quickstart
- [x] T039 Make the C++ token-bootstrap shell regression self-contained by starting and cleaning up NFD when needed
- [x] T040 Add requested identity to the certificate bootstrap request TLV.
- [x] T041 Enforce request-name, request-payload identity, token-table identity, and certificate identity matching in ServiceController.
- [x] T042 Add requested-identity binding under the bootstrap token flow while avoiding duplicate name arguments in the public Python API.
- [x] T043 Update regressions and MiniNDN token bootstrap to use identity-derived name plus token pairs.
- [x] T044 Align the Python object/config API with ndncert-style token usage: identity is configured once, token proves authorization for that identity.
- [x] T045 Add NDNCERT token challenge documentation and shared token-file format coverage.
- [x] T046 Add encrypted certificate bootstrap request envelope and RSA-wrapped AES-CBC helpers.
- [x] T047 Make User/Provider bootstrap encrypt request payloads to the Controller certificate and include requester proof signatures.
- [x] T048 Make ServiceController decrypt bootstrap requests and verify requester proof signatures before certificate issuance.
- [x] T049 Update certificate bootstrap spec/contract/data model for encrypted token-bearing requests.
- [x] T050 Add tampered requester-proof regression that verifies ServiceController rejects `request-proof-invalid` without changing the configured identity-token map.
- [x] T051 Add the token certificate bootstrap regression to the common security regression entrypoint and documentation.
- [x] T052 Make the ServiceController identity-token map stable across successful issuance and add repeated valid-probe coverage.
- [x] T053 Generate a missing ServiceController bootstrap token file from policy identities with persisted 8-character tokens and regression coverage.

Validation note: NDNSF builds and the MiniNDN token-bootstrap smoke test passed.
The local ndncert checkout was adjusted to the Ubuntu 20.04 Boost 1.71 baseline,
and the ndncert token challenge unit test plus full ndncert unit suite passed.
