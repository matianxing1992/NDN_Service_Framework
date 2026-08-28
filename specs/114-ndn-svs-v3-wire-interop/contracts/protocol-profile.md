# Contract: Protocol Profile Selection

## Public shape

```cpp
enum class SvsProtocolVersion : uint8_t {
  V2 = 2,
  V3 = 3,
};

struct SyncProtocolOptions {
  SvsProtocolVersion version = SvsProtocolVersion::V3;
  std::optional<BootstrapTime> bootstrapTime;
  std::optional<time::milliseconds> syncInterestLifetime;
  std::optional<time::milliseconds> suppressionPeriod;
};
```

The concrete spelling may follow project naming conventions, but the observable
contract and one-source-of-truth ownership must remain.

## Propagation

- `SVSyncCore` accepts one immutable `SyncProtocolOptions` at construction.
- `SVSyncBase`, `SVSync`, and `SVSyncShared` forward the same value without
  resolving their own defaults.
- `SVSPubSubOptions` contains one `syncProtocol` field and forwards it to its
  internal `SVSync`; it does not duplicate version/timer fields.
- Existing constructor calls remain source-compatible through a trailing
  defaulted options argument or the existing `SVSPubSubOptions` default.
- Binary ABI compatibility is not claimed; all dependent C++ consumers must be
  rebuilt against the selected candidate headers and library.
- Existing default wire behavior intentionally changes from the hybrid/V2 path
  to complete V3. Applications needing V2 must select it explicitly.

## Resolved defaults

| Property | V2 | V3 |
|---|---:|---:|
| version component | 2 | 3 |
| authoritative signed object | outer Interest | embedded State Vector Data |
| default Interest lifetime | 1 ms | 1000 ms |
| default suppression period | 500 ms | 200 ms |
| periodic timeout | 30 s ±10% | 30 s ±10% |
| raw StateVector parameters | yes | prohibited |
| whole-parameters LZMA | retained build behavior | prohibited unless future profile |

An explicit timer override changes only that timer. It must be exposed in a
startup diagnostic and candidate manifest. It cannot alter packet framing.

## Routing and mixed versions

- V2 registers `/<group-prefix>/v=2`.
- V3 registers `/<group-prefix>/v=3`.
- Registration on only `/<group-prefix>` is prohibited for version inference.
- A participant sends only one version and consumes only its selected version.
- No automatic downgrade, dual publication, cross-version state merge, or
  transparent retry is allowed.
- A wrong-version packet reaching a participant through manual injection is
  rejected and increments `syncVersionMismatch` or an equivalent diagnostic.
- Natural V2/V3 route isolation means a participant may not receive the other
  version. Each participant therefore logs its selected profile at startup, and
  the interop harness diagnoses an incompatible pair from both declarations.

## NDNSF selection

NDNSF maps `NDNSF_SVS_PROTOCOL_VERSION=v2|v3` into the same
`SVSPubSubOptions::syncProtocol.version` field for ServiceUser and
ServiceProvider. Missing or invalid values resolve to V3 or fail startup
respectively; there is no silent fallback. README and README_ch document the
same contract.

`NDNSF_SVS_MAX_SUPPRESSION_MS` is an override, not an NDNSF-owned default.
ServiceUser and ServiceProvider call `setMaxSuppressionTime()` only when this
environment variable is present and valid. If it is absent, the selected
profile resolves its own suppression period (200 ms for V3, 500 ms for V2).
An explicit historical 1 ms value remains legal and is logged as an override.
The shared GUI adapter defaults this field to empty and emits it only when the
operator supplies a value; it also carries the explicit `v2|v3` selector.

## Bootstrap ownership

- A supplied bootstrap time is validated before route registration.
- A valid supplied value is reused exactly.
- An absent value resolves to current Unix time in seconds.
- The active value remains available through `getBootstrapTime()`.
- Persistence location, lifecycle, and atomic storage remain caller policy.

## Compatibility exit

Spec 114 does not schedule V2 deletion. Future deletion requires evidence that
all declared C++ consumers can run V3 and a separate compatibility-removal spec.
