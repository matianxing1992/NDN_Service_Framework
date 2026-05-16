````md
# NDNSF Refactor Rules

## Core Direction

The framework is migrating from generated static service/stub code to a generic dynamic C++ API.

Preferred provider-side API:

```cpp
provider.addHandler<RequestT, ResponseT>(
    ndn::Name("/ObjectDetection/YOLOv8"),
    handler);
````

Preferred user-side API:

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

Generated:

* `ServiceUser_*`
* `ServiceProvider_*`
* `*Service`
* `*ServiceStub`

are legacy compatibility code only and should not be treated as the primary architecture target.

---

# Unified serviceName Rule

Use unified `serviceName` only.

`serviceName` is the complete endpoint path, for example:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
```

Do not design new APIs around separate:

```text
ServiceName + FunctionName
```

All new logic should treat the full endpoint path as a single service identifier.

---

# V2 Naming Rules

All new generic runtime paths should use V2 naming helpers.

V2 names use:

* explicit component counts
* unified `serviceName`
* variable-length endpoint paths

Use V2 helpers for:

* request names
* response names
* ACK names
* coordination names
* permission token names

Do not add new logic based on old split-name helpers.

Old naming helpers remain only for legacy compatibility.

---

# V2 Wire Formats

## Request

```text
/<requester>/NDNSF/REQUEST/<serviceComponentCount>/<serviceName...>/<bloomFilter>/<requestId>
```

Example:

```text
/muas/user/NDNSF/REQUEST/2/ObjectDetection/YOLOv8/<bloom>/<requestId>
```

---

## Response

```text
/<provider>/NDNSF/RESPONSE/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
```

---

## ACK

```text
/<provider>/NDNSF/ACK/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
```

---

## Coordination

```text
/<requester>/NDNSF/COORDINATION/<providerComponentCount>/<provider...>/<serviceComponentCount>/<serviceName...>/<requestId>
```

---

# Framework Core Ownership

The generic dynamic runtime belongs in:

```text
ndn-service-framework/ServiceUser.hpp
ndn-service-framework/ServiceUser.cpp
ndn-service-framework/ServiceProvider.hpp
ndn-service-framework/ServiceProvider.cpp
```

Generated files should shrink over time.

---

# ServiceProvider Responsibilities

`ServiceProvider` should own:

* dynamic handler registry
* `addHandler<RequestT, ResponseT>`
* unified `serviceName` dispatch
* provider-side ACK logic
* provider-side response publication
* request-name parsing
* V2 request/response/ACK support

Provider dispatch should no longer depend on generated service-specific branching.

---

# ServiceUser Responsibilities

`ServiceUser` should own:

* `asyncCall<RequestT, ResponseT>`
* pending callback registry
* response dispatch
* ACK storage
* ACK selection skeleton
* custom `AcksHandler`
* V2 request/response/ACK support

Generated stubs should eventually become thin wrappers or disappear.

---

# ACK Strategy Rules

ACK processing has two layers:

1. custom `AcksHandler`
2. built-in fallback strategy

Priority:

```text
custom AcksHandler
    >
built-in strategy
```

Built-in strategies:

* `FirstResponding`
* `LoadBalancing`
* `NoCoordination`

`NoCoordination` may accept multiple provider responses.

ACK logic should remain generic and service-independent.

---

# Publish Boundary

`PublishMessageBridge` is the boundary between:

```text
generic RPC layer
```

and:

```text
runtime transport/security backend
```

Architecture:

```text
ServiceUser / ServiceProvider
    ↓
PublishMessageBridge
    ↓
RuntimeBackend
    ↓
NAC-ABE + IMS + SVS
```

Do not move NAC/SVS/IMS lifecycle into generated files.

---

# Runtime Backend Ownership

Runtime backend should own:

* Face
* IMS
* validators
* NAC producer/consumer
* SVS publisher/subscriber
* signing info
* KeyChain
* trust schema
* session state

Generated RPC layers should not own runtime transport/security lifecycle.

---

# Permission Architecture

NDNSD is no longer the core permission dependency.

Permissions are fetched directly from `ServiceController`.

Permission Interest names:

```text
/<controller>/NDNSF/PERMISSIONS/USER/<targetIdentity...>
/<controller>/NDNSF/PERMISSIONS/PROVIDER/<targetIdentity...>
```

---

# Permission Security Model

Permission responses are:

* controller-signed
* encrypted to the target identity certificate public key

They are NOT encrypted with NAC-ABE.

Current intended encryption:

```text
RSA-wrapped AES-CBC
```

Security model:

```text
controller signs Data
target identity decrypts payload
```

Interest signature verification is optional for confidentiality.

The controller must never trust arbitrary public keys supplied by Interests.

It must use trusted certificates for target identities.

---

# Permission Messages

## PermissionEntry

Fields:

```text
providerName
serviceName
token
ttl
version
```

`serviceName` is unified.

No `FunctionName`.

---

## PermissionResponse

Fields:

```text
targetIdentity
permissionKind
PermissionEntry*
```

Kinds:

```text
USER
PROVIDER
```

---

## EncryptedPermissionResponse

Fields:

```text
RecipientCertName
Algorithm
EncryptedAesKey
Iv
CipherText
```

Used only for permission encryption.

Not NAC-ABE.

---

# Permission Fetch Flow

## ServiceController

Flow:

```text
build PermissionResponse
→ encrypt to target cert
→ wrap in EncryptedPermissionResponse
→ sign Data
→ reply
```

---

## ServiceUser / ServiceProvider

Flow:

```text
receive Data
→ validate controller signature
→ decode EncryptedPermissionResponse
→ decrypt with private key
→ decode PermissionResponse
→ verify targetIdentity
→ verify permissionKind
→ applyPermissionResponse
```

---

# Permission Tables

UPT entries should use unified service names.

Canonical mapping:

```text
/<provider>/<serviceName...>
    ->
(serviceName, token)
```

No split `ServiceName/FunctionName`.

---

# NAC-ABE Scope

NAC-ABE remains for:

* NDNSF service request messages
* NDNSF service response messages
* future distributed coordination payloads

PermissionResponse encryption is NOT NAC-ABE.

---

# Testing Rules

Prefer small regression tests before large runtime changes.

Tests should follow Boost unit-test style similar to ndn-cxx.

Primary current tests:

```text
tests/unit-tests/encrypted-permission-response.t.cpp
```

Tests should cover:

* V2 request/response/ACK parsing
* unified service names
* encrypted permission response flow
* generic dynamic API
* `addHandler`
* `asyncCall`
* ACK selection skeleton
* local/mock runtime flow

Prefer local/in-process tests before real network integration.

---

# Refactor Rules

* Keep changes incremental.
* Do not rewrite the whole framework at once.
* Preserve working tests.
* Do not fix unrelated errors unless they block the current task.
* Prefer generic APIs over generated wrappers.
* Do not add new service-specific generated dispatch logic.
* Generated files should shrink over time.
* Avoid large architectural jumps in one step.

---

# Current Runtime Status

Already implemented:

* generic dynamic provider registry
* generic async call skeleton
* V2 naming helpers
* ACK selection skeleton
* encrypted permission response flow
* plaintext fallback compatibility
* encrypted permission response tests
* controller-side permission response generation
* local/mock permission callback tests

Still incomplete:

* full runtime backend integration
* real NAC-ABE publish/decrypt bridge
* SVS runtime integration cleanup
* generic runtime example without generated stubs
* removal of legacy generated compatibility layers
* real timeout scheduler integration
* full network integration tests

---

# Current Priority

Current next goal:

Add local/mock regression tests proving:

```text
ServiceProvider::addHandler<RequestT, ResponseT>
ServiceUser::asyncCall<RequestT, ResponseT>
```

work correctly without generated stubs.

Focus on:

* generic dispatch
* V2 request/response naming
* pending callback registry
* typed response callback
* optional ACK skeleton

Do not work on NDNSD migration now.
Do not expand generated compatibility layers now.

```
```
