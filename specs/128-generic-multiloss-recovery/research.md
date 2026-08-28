# Research: Spec 128 Decisions

## D1: Retry the same authenticated name

Use the existing Mapping-bound exact name and bounded cursor attempt state. A
new name or application retry hook would widen the security surface and violate
generic ownership.

## D2: Two independent generic repair symbols

Use systematic GF(256) parity with a declared capacity of two. It recovers any
two opaque missing source symbols; repeated XOR cannot. Keep existing one-XOR
definitions untouched and do not depend on a codec library.

## D3: New evidence, never replacement evidence

Freeze a fresh 16-cell MiniNDN manifest. Preserve every failure. Spec 127 is
hashed historical context only, never a new-code treatment/control repetition.
