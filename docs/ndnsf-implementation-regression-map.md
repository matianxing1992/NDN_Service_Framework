# NDNSF Implementation and Regression Map

This map ties the current implementation and verified regressions to the main
NDNSF paper contributions. It reflects the dynamic runtime API and V2 naming
paths used by the checked-in examples and tests.

Verified commands and scripts:

- `examples/run_hello_auth_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `examples/run_nac_abe_attribute_routing_regression.sh`
- `./build/unit-tests`

## 1. Mutual Authentication / Trust

Relevant source files:

- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/ServiceController.cpp`
- `ndn-service-framework/NDNSFMessages.hpp`
- `ndn-service-framework/NDNSFMessages.cpp`
- `ndn-service-framework/utils.hpp`
- `ndn-service-framework/utils.cpp`
- `examples/App_ServiceController.cpp`
- `examples/App_Provider.cpp`
- `examples/App_User.cpp`
- `examples/trust-any.conf`
- `examples/trust-schema.conf`

Relevant regression/unit tests:

- `examples/run_hello_auth_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `./build/unit-tests`

What is verified:

- The controller, user, and provider run with identities and certificates
  created by the example apps.
- The user and provider fetch controller-issued permission mappings before executing
  the `/HELLO` service flow.
- `ServiceUser` generates a one-time `UserToken`, providers echo it in ACK and
  response messages, and users reject mismatches.
- `ServiceProvider` generates a one-time `ProviderToken` in ACK messages, and
  coordination-based execution requires the selected provider's token.
- The regressions guard against debug authorization bypasses by checking logs
  and source for `isAuthorized = true`.
- Unit tests exercise dynamic request handling, V2 message parsing, permission
  application, and negative token-handshake behavior.

Remaining assumptions or limitations:

- The example regressions use local example trust configuration; they are
  regression coverage for the framework flow, not a complete production PKI
  deployment.
- The scripts verify successful trust and authorization paths mostly through
  runtime logs.
- Broader adversarial cases, such as replay, compromised keys, or malicious
  controller behavior, are outside these regressions.

## 2. Access Control and Permission Discovery

Relevant source files:

- `ndn-service-framework/ServiceController.hpp`
- `ndn-service-framework/ServiceController.cpp`
- `ndn-service-framework/ServiceUser.hpp`
- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/ServiceProvider.hpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/UserPermissionTable.hpp`
- `ndn-service-framework/PolicyParser.hpp`
- `ndn-service-framework/PolicyParser.cpp`
- `examples/hello.policies`
- `examples/App_ServiceController.cpp`
- `examples/App_Provider.cpp`
- `examples/App_User.cpp`

Relevant regression/unit tests:

- `examples/run_hello_auth_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `./build/unit-tests`
- `examples/run_token_handshake_negative_regression.sh`

What is verified:

- Users fetch permissions from
  `/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity>`.
- Providers fetch permissions from
  `/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity>`.
- `ServiceController` parses the target identity from the permission discovery
  Interest name and builds the matching user or provider `PermissionResponse`.
- `ServiceUser` and `ServiceProvider` install permission entries keyed by
  unified service names, for example `/example/hello/provider/HELLO`.
- The multi-provider selective ACK regression verifies permission discovery and
  installation for providers A, B, and C.
- Unit tests include unsigned permission discovery Interest coverage and
  permission response target/kind rejection coverage.

Remaining assumptions or limitations:

- Permission discovery Interests are intentionally unsigned in the current
  design; authenticity is provided by controller-signed Data and target-only
  encrypted payloads.
- The permission table is an in-memory runtime structure.
- Policy expressiveness is limited to the current parser and example policy
  files exercised by the tests.

## 3. PermissionResponse Encryption and Controller-Signed Data

Relevant source files:

- `ndn-service-framework/NDNSFMessages.hpp`
- `ndn-service-framework/NDNSFMessages.cpp`
- `ndn-service-framework/utils.hpp`
- `ndn-service-framework/utils.cpp`
- `ndn-service-framework/ServiceController.cpp`
- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/ServiceProvider.cpp`

Relevant regression/unit tests:

- `examples/run_hello_auth_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `tests/unit-tests/encrypted-permission-response.t.cpp`
- `./build/unit-tests`

What is verified:

- `PermissionResponse` and `EncryptedPermissionResponse` wire encode/decode.
- Permission responses are encrypted for the target identity certificate using
  the current RSA-wrapped AES-CBC helper.
- The controller signs permission Data under the controller identity before
  publishing it.
- User and provider runtimes decrypt encrypted permission responses and reject
  responses with the wrong target identity or permission kind.
- Unit tests verify that a non-target keychain cannot decrypt the encrypted
  permission response.
- Runtime regressions verify controller logs for encrypted user and provider
  permission replies.

Remaining assumptions or limitations:

- The encryption helper is specific to `PermissionResponse`; it is explicitly
  separate from NAC-ABE service message protection.
- The regressions check encrypted reply logs and successful installation, while
  detailed cryptographic failure cases are covered by unit tests.
- The current helper requires RSA recipient certificates.

## 4. NAC-ABE Service-Message Protection

Relevant source files:

- `ndn-service-framework/utils.hpp`
- `ndn-service-framework/utils.cpp`
- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/nac-abe-config.hpp`
- `ndn-service-framework/NDNSFMessages.hpp`
- `ndn-service-framework/NDNSFMessages.cpp`

Relevant regression/unit tests:

- `examples/run_nac_abe_attribute_routing_regression.sh`
- `examples/run_hello_auth_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `./build/unit-tests`

What is verified:

- V2 service message names use counted service and identity components.
- `GetAttributesByName` maps V2 `REQUEST` and `COORDINATION` messages to
  `/SERVICE/<service>`.
- `GetAttributesByName` maps V2 `ACK` and `RESPONSE` messages to
  `/PERMISSION/<service>`.
- `ServiceUser::PublishMessage` and `ServiceProvider::PublishMessage` use the
  derived attributes when publishing NAC-ABE protected NDNSF messages.
- The NAC-ABE attribute routing regression verifies runtime logs for
  `/SERVICE/HELLO` on request/coordination and `/PERMISSION/HELLO` on
  ACK/response.
- Unit tests cover V2 name construction/parsing used by the attribute routing
  logic.

Remaining assumptions or limitations:

- The routing regression verifies attribute selection through logs, not by
  independently decrypting every protected packet in the script.
- Legacy split service/function name routing remains present for compatibility.
- Permission response Data is intentionally not NAC-ABE encrypted; it uses the
  target-certificate encryption path described above.

## 5. Provider-Side Selective ACK

Relevant source files:

- `ndn-service-framework/ServiceProvider.hpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/NDNSFMessages.hpp`
- `ndn-service-framework/NDNSFMessages.cpp`
- `examples/App_Provider.cpp`

Relevant regression/unit tests:

- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `tests/unit-tests/generic-dynamic-api.t.cpp`
- `tests/unit-tests/ndn-svs-smoke.t.cpp`
- `./build/unit-tests`

What is verified:

- Providers can register a service-level ACK handler that returns an
  `AckDecision`.
- The provider publishes `RequestAckMessage` with status, message, and optional
  payload metadata, plus `UserToken` and `ProviderToken`.
- The selective ACK regression starts three providers and verifies that A and B
  publish successful ACKs while C rejects the request.
- The provider saves authorized requests for later execution after coordination
  when coordination-based strategies are used.
- Legacy ACK handler compatibility remains available through wrapper APIs.

Remaining assumptions or limitations:

- The regression validates selective ACK decisions for `/HELLO`; it does not
  exhaustively cover every service payload type.
- Provider ACK decisions are application supplied, so correctness of metadata
  semantics is left to the application.
- Legacy generated provider paths remain compatibility code and are not the
  focus of these regressions.

## 6. ACK Payload Metadata

Relevant source files:

- `ndn-service-framework/NDNSFMessages.hpp`
- `ndn-service-framework/NDNSFMessages.cpp`
- `ndn-service-framework/ServiceProvider.hpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/ServiceUser.cpp`
- `examples/App_Provider.cpp`
- `examples/App_User.cpp`

Relevant regression/unit tests:

- `examples/run_hello_ack_payload_regression.sh`
- `examples/run_selective_ack_custom_selection_regression.sh`
- `tests/unit-tests/generic-dynamic-api.t.cpp`
- `tests/unit-tests/ndn-svs-smoke.t.cpp`
- `./build/unit-tests`

What is verified:

- `RequestAckMessage` carries a binary payload alongside status and message.
- `RequestAckMessage` carries `UserToken` and `ProviderToken` alongside
  status, message, and payload.
- `ServiceProvider::AckDecision` carries ACK metadata as an `ndn::Buffer`.
- `PublishRequestAckMessageV2` copies ACK payload bytes into the published
  `RequestAckMessage`.
- `ServiceUser` logs received ACK status, message, payload bytes, and provider
  identity, plus the token fields used by the handshake.
- The HELLO ACK payload regression verifies metadata
  `queue=0;gpu=idle;model=hello-v1` from provider publication through user
  collection.
- The selective ACK custom selection regression verifies distinct metadata from
  providers A and B, including queue and rank fields.

Remaining assumptions or limitations:

- ACK payload contents are opaque to the framework and interpreted by
  application code.
- Current regressions use text metadata, though the framework payload field is
  binary.
- Metadata freshness and truthfulness are not independently attested beyond the
  provider's authenticated, authorized message flow.

## 7. Information-Aware Custom Selection

Relevant source files:

- `ndn-service-framework/ServiceUser.hpp`
- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/NDNSFMessages.hpp`
- `examples/App_User.cpp`

Relevant regression/unit tests:

- `examples/run_selective_ack_custom_selection_regression.sh`
- `examples/run_hello_ack_payload_regression.sh`
- `tests/unit-tests/generic-dynamic-api.t.cpp`
- `tests/unit-tests/ndn-svs-smoke.t.cpp`
- `./build/unit-tests`

What is verified:

- Custom selection receives a vector of `AckSelectionCandidate` values that
  include provider name, service name, request ID, and full `RequestAckMessage`.
- Selection is timeout-driven: the user collects ACKs during `ackTimeoutMs`,
  then invokes the custom strategy.
- Candidate matching compares provider name, service name, request ID, ACK
  status, ACK message, and full ACK payload bytes before coordinating selected
  candidates.
- The selective ACK custom selection regression verifies the custom strategy ran
  after `ackTimeoutMs=500`, inspected provider metadata, rejected C, and selected
  provider B based on rank/queue metadata.
- The HELLO ACK payload regression verifies a simpler metadata-aware selection
  path using `model=hello-v1`.

Remaining assumptions or limitations:

- The framework supplies candidates and coordinates selected providers; the
  application owns the ranking policy.
- The scripts verify timeout-driven behavior through runtime logs rather than a
  deterministic simulated clock.
- Current examples select one provider, though the candidate handler type can
  return multiple selected candidates.

## 8. Selected-Provider-Only Execution

Relevant source files:

- `ndn-service-framework/ServiceUser.cpp`
- `ndn-service-framework/ServiceProvider.cpp`
- `ndn-service-framework/utils.hpp`
- `ndn-service-framework/utils.cpp`
- `examples/App_Provider.cpp`
- `examples/App_User.cpp`

Relevant regression/unit tests:

- `examples/run_selective_ack_custom_selection_regression.sh`
- `examples/run_hello_auth_regression.sh`
- `tests/unit-tests/generic-dynamic-api.t.cpp`
- `tests/unit-tests/ndn-svs-smoke.t.cpp`
- `./build/unit-tests`

What is verified:

- `ServiceUser` publishes a V2 coordination message only for ACK candidates
  selected by the built-in or custom strategy.
- `ServiceProvider` consumes V2 coordination messages only when the provider name
  in the coordination name matches its own identity.
- The selected provider dispatches the saved pending request and publishes the
  final V2 response.
- The selective ACK custom selection regression verifies that provider B
  publishes `HELLO_FROM_B`, while providers A and C do not publish final
  responses.
- User response handling verifies receipt of the selected provider's response.

Remaining assumptions or limitations:

- The selected-provider-only regression covers the coordination path. The
  `NoCoordination` strategy intentionally permits direct response execution and
  is a different behavior.
- The script verifies non-execution by absence of provider A/C final response
  log lines.
- The current example checks a single selected provider; multi-selection
  behavior is supported by the handler type but is not the primary regression.
