# Spec182 R11-B1-PY Native Binding Authority Boundary

Date: 2026-09-10
Branch: `Experimental`
Source baseline: `d658d238`
Decision: `CLOSED_FOR_VALIDATION` (Python binding ownership boundary only)

## Boundary

The C++ `_ndnsf` binding previously accepted authority signing/content-key paths,
loaded those files in the requester process, constructed `NativeArtifactGrantIssuer`,
and passed the local issuer into `NativeAuthenticatedGrantClient`. That contradicted
the R11 independent-authority contract even though the standalone C++ requester had
already moved to Core authority transport.

The repair makes the binding follow the same native boundary:

- `NativeServiceUser::nativeGrantClientFromConfig` accepts only the requester private
  key, authority public key, authority service, authority identity, and protection epoch;
- authority private key, content key, requester public key, recipient registry, and
  publication-policy fields are rejected before any requester key loading;
- the binding constructs the transport-only C++ client with
  `NativeAuthenticatedGrantClient::issueThroughCore` and `publishThroughCore`;
- `APPClient.configure_native_requester_from_config` rejects authority-owned fields and
  forwards only the public-key transport configuration;
- the service wrapper documentation now describes a requester-side client rather than
  an authority key owner.

No Python planner, grant issuer, or content-key fallback was added.

## Review trace

The official `/home/tianxing/.codex/skills/review-agent/SKILL.md` was applied as a
read-only defect-first review (SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`).
Coverage included the complete diff, the C++ binding call site, `NativeAuthenticatedGrantClient`
constructor overloads, the Python config loader, service wrapper, tests, and the requester
configuration contract. No actionable finding remained after the review; the only initial
warning was an unused C++ helper left behind by removing the local issuer, which was removed
and rebuilt.

## Validation

| Lane | Result |
| --- | --- |
| production entry/callers | C++ `NativeServiceUser::nativeGrantClientFromConfig` and Python `APPClient.configure_native_requester_from_config` both use the independent authority transport path; forbidden authority-owned fields fail closed |
| implementation/wire | system-first extension build linked `issueThroughCore`/`publishThroughCore`; no local `NativeArtifactGrantIssuer` remains in the binding path |
| C++ test/harness/oracle | refreshed `ndnsf-distributed-inference` library build: 96/96 tasks, 5m12.212s, exit 0; `Spec182NativeInferenceClient` 2 cases, configured client 1 case, `Spec182ProviderHost` 6 cases, and `Spec170NdnsfDiCoreFlow/Spec182*` 7 integration cases passed |
| Python wrapper | `py_compile` passed; native extension imported; 72 Spec182 wrapper/contract tests passed; actual binding rejected an authority-private-key config and constructed a valid public-key/Core-transport client |
| build/source closure | extension rebuilt with `/home/tianxing/NDN/nac-abe-integration-182/install` (the exact NAC-ABE dependency of `build-nac182`), `build-nac182` C++ library, system `/usr/bin/g++`; `readelf` RUNPATH points at the current candidate library and NAC-ABE prefixes |

Raw logs:

- `.codex-tmp/spec182-r11-b8-authority/native-library-build-current.log`
- `.codex-tmp/spec182-r11-b8-authority/extension-build-4.log`
- `.codex-tmp/spec182-r11-b8-authority/cpp-native-client.log`
- `.codex-tmp/spec182-r11-b8-authority/cpp-client-state.log`
- `.codex-tmp/spec182-r11-b8-authority/cpp-provider-host.log`
- `.codex-tmp/spec182-r11-b8-authority/cpp-integration-selector.log`

The first extension retry used `/usr/local/lib/libnac-abe.so`; `ldd -r` then exposed
the first native boundary as an undefined `ndn::nacabe::Consumer::clearCache` symbol because
that library did not match the current `build-nac182` framework. The corrected rebuild used
the matching integration-182 install prefix and imported successfully. This is a dependency
selection boundary, not a product behavior result.

## Remaining scope

This closes the binding's authority ownership seam only. It does not close the 16 maintained
caller migrations, no-Python/default-route retirement, dependency/container closure, T015/T016,
or T017; R11-B8 and R11-B9 remain open and no parent task is advanced.
