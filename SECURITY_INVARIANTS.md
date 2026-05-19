# NDNSF Security Invariants

This document records security properties that the runtime must preserve. These are requirements, not optional implementation notes.

## Authentication

- `PermissionResponse` Data must be validated with the configured trust validator before it is decrypted or installed.
- The signer identity of `PermissionResponse` Data must match the Permission Controller identity encoded in the permission Interest path.
- `ACK`, `COORDINATION`, and `RESPONSE` messages must pass the configured trust validation and NAC-ABE authorization flow before their payloads affect runtime state.
- Permission discovery Interests may remain unsigned; the returned Data is the authenticated object.

## Token Properties

- `ProviderToken` is one-time use.
- `ProviderToken` expires after the provider-side pending state TTL.
- Replaying a `ProviderToken` after successful coordination must fail.
- Using an expired `ProviderToken` must fail.
- Injecting an unknown or random `ProviderToken` must fail.
- A provider restart before coordination must not preserve pending `ProviderToken` state unless an explicit persistence mechanism is introduced and audited.

## State Properties

- `pendingRequests` and `pendingProviderTokens` must eventually be cleaned.
- Successful coordination completion removes provider pending state immediately.
- Timeout cleanup must not remove active requests before normal coordination can arrive.
- Cleanup that fires after successful completion must be a no-op.
- Repeated cleanup cycles must not grow pending state without bound.

## Failure Properties

- Validator failures must invoke explicit failure callbacks exactly once.
- Validator failures must log the failed name and reason.
- Decryption failures must not mutate request, permission, token, or completion state.
- Malformed payloads must not mutate request, permission, token, or completion state.
- Negative paths must fail closed: no permission install, no token consumption, no request execution, and no final response publication.
