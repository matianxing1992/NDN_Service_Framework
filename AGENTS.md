# NDNSF Design Notes

## Core Direction

The framework is moving from generated static service/stub code to a generic dynamic C++ API.

Preferred provider API:

```cpp
provider.addHandler<RequestT, ResponseT>(
    ndn::Name("/ObjectDetection/YOLOv8"),
    handler);
```

Preferred user API:

```cpp
user.asyncCall<RequestT, ResponseT>(
    providers,
    ndn::Name("/ObjectDetection/YOLOv8"),
    request,
    onResponse,
    onTimeout,
    timeoutMs,
    strategy);
```

Generated `ServiceUser_*`, `ServiceProvider_*`, `*Service`, and `*ServiceStub` files are legacy compatibility code only.

## Examples and CodeGenerator Outputs

Examples should demonstrate the current dynamic runtime API:

```cpp
provider.addService(ndn::Name("/HELLO"), ackHandler, requestHandler);
provider.addHandler<RequestT, ResponseT>(serviceName, handler);
user.async_call(serviceName, request, ackTimeoutMs, selector, timeoutMs, onTimeout, onResponse);
user.asyncCall<RequestT, ResponseT>(providers, serviceName, request, onResponse, onTimeout, timeoutMs, strategy);
```

Do not add new examples that depend on generated service users, generated service providers, generated service/stub classes, or service-specific framework message types. Application data belongs in `RequestMessage.payload` and `ResponseMessage.payload`, or in app-defined typed request/response objects wrapped by the generic helpers.

Checked-in `CodeGenerator/Generated` C++ outputs are not required unless a build target, test, or intentionally retained compatibility example uses them. Prefer keeping generator scripts/templates for migration reference and deleting stale generated outputs.

## Unified Service Names

New runtime logic uses one unified `serviceName`.

Examples:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
/HELLO
```

Do not design new APIs around split `ServiceName + FunctionName`. Old helpers remain only for compatibility.

## HELLO Payload Model

HELLO is application payload only. There are no framework-level HELLO-specific request, response, or message TLV types.

Example:

```text
RequestMessage.payload = "HELLO"
ResponseMessage.payload = "HELLO"
```

The HELLO examples use normal `RequestMessage`, `RequestAckMessage`, and `ResponseMessage` payload fields.

## V2 Naming

All new generic runtime paths should use V2 helpers with explicit component counts.

Request:

```text
/<requester>/NDNSF/REQUEST/<service-component-count>/<service-name...>/<bloomFilter>/<requestId>
```

Example:

```text
/example/hello/user/NDNSF/REQUEST/1/HELLO/<bloomFilter>/<requestId>
```

Response:

```text
/<provider>/NDNSF/RESPONSE/<requester-component-count>/<requester...>/<service-component-count>/<service-name...>/<requestId>
```

ACK:

```text
/<provider>/NDNSF/ACK/<requester-component-count>/<requester...>/<service-component-count>/<service-name...>/<requestId>
```

Coordination:

```text
/<requester>/NDNSF/COORDINATION/<provider-component-count>/<provider...>/<service-component-count>/<service-name...>/<requestId>
```

## NAC-ABE Routing

NAC-ABE policy routing uses service-level namespaces:

```text
REQUEST
COORDINATION
    -> /SERVICE/<service>

ACK
RESPONSE
    -> /PERMISSION/<service>
```

Permission responses themselves are not NAC-ABE encrypted. They are controller-signed and encrypted to the target identity certificate.

## Selective ACK Design

Providers can publish ACK metadata with `AckDecision`:

```cpp
struct AckDecision {
    bool status = false;
    std::string message;
    ndn::Buffer payload;
};
```

ACK payload examples:

```text
queue length
waiting time
GPU utilization
RTT
model availability
```

New provider ACK handlers use:

```cpp
using AckStrategyHandler =
    std::function<AckDecision(const RequestMessage&)>;
```

Legacy ACK handlers remain source-compatible:

```cpp
using LegacyAckStrategyHandler =
    std::function<std::pair<bool, ndn::Block>(const RequestAckMessage&)>;
```

Legacy handler results are wrapped internally:

```text
pair<bool, Block>
    -> AckDecision{status=result.first, payload=wire bytes of result.second}
```

Providers may register new or legacy handlers with:

```cpp
setAckStrategyHandler(serviceName, handler);
setLegacyAckStrategyHandler(serviceName, legacyHandler);
```

Existing `addService(..., legacyHandler, ...)` call sites should continue to compile.

## Custom Selection Strategy

Custom selection is timeout-driven, not ACK-driven.

Flow:

```text
User sends request
Collect ACKs during ackTimeoutMs
No custom selection on each ACK
After timeout:
    strategyHandler(vector<AckSelectionCandidate>)
```

`AckSelectionCandidate` contains:

```text
providerName
serviceName
requestId
RequestAckMessage
```

`RequestAckMessage` contains:

```text
status
message
payload
```

Candidate matching must compare:

```text
status
message
full payload bytes
```

Comparing only payload size is ambiguous and must not be used.

## Built-In ACK Strategies

Built-in strategies remain generic and service-independent:

```text
FirstResponding
LoadBalancing
NoCoordination
```

`FirstResponding` should coordinate the first successful ACK as before. `NoCoordination` may accept multiple provider responses.

## Permission Architecture

Permissions are fetched directly from `ServiceController`.

Permission Interest names:

```text
/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity...>
/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity...>
```

Permission table entries use unified service names:

```text
/<provider>/<serviceName...> -> (serviceName, token)
```

`ServiceController` changes that support the current HELLO auth regression are intentional:

```text
signed permission Interests
parameters digest stripping
target-certificate encrypted PermissionResponse payloads
explicit start() registration path
demo authorization tokens consumed by ServiceUser and ServiceProvider
```

These are permission/auth flow requirements, not ACK payload requirements. Do not mix additional controller refactors into ACK-only changes.

## HELLO Regression Flow

The HELLO regression exercises the generic runtime without framework-specific HELLO wire types.

Flow:

```text
App_ServiceController starts with examples/hello.policies
App_Provider fetches provider permissions
App_User fetches user permissions
User publishes RequestMessage(payload="HELLO") to /HELLO
Provider validates authorization proof
Provider publishes RequestAckMessage(status=true, message, payload=provider metadata)
User collects ACK candidates during ackTimeoutMs
User selects a provider after the timeout
Provider publishes ResponseMessage(payload="HELLO")
User receives the HELLO response
```

Current regression scripts:

```text
examples/run_hello_auth_regression.sh
examples/run_hello_ack_payload_regression.sh
```

## Refactor Rules

Keep changes incremental. Preserve legacy compatibility unless a migration is explicitly planned. Do not rewrite generated compatibility layers for new generic behavior. Do not modify `ServiceController` for ACK-only work unless the permission/auth regression requires it and the reason is documented here.
