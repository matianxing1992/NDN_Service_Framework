# Contract: Compatibility and Migration

## Principle

Spec 116 establishes a preferred API; it does not delete the Spec 111
compatibility manifest. Compatibility code must be a one-way delegate into the
canonical owner, never a second behavior implementation.

## Migration Map

| Current surface | Preferred replacement | Compatibility behavior |
|---|---|---|
| root-level imports | `ndnsf_distributed_inference.api` or `.sdk` | Lazy delegate and one actionable warning |
| `APPClient.distributed_inference` | `InferenceClient.run` | Compose canonical `request().result()` |
| `async_distributed_inference` / `infer_async` | `InferenceClient.request` | Return/bridge canonical request handle |
| `infer` | `InferenceClient.run` | Strict delegate |
| overloaded positional local `submit` | explicit `LocalInferenceHarness.request` | No production network branch |
| numeric/heuristic deadline | typed `timeout` or aware `deadline` | Parse only in adapter and warn |
| client `stream` lifecycle method | request `events` | Alias without claiming model output streaming |
| raw deployment discovery functions | `client.deployments.discover/get` | Convert typed ACTIVE/ON_DEMAND result to legacy dictionary only for old caller |
| provider `serve_service` | provider `serve` | Strict delegate |
| direct provider lifecycle methods | separately constructed advanced `ProviderAdminPort` | Existing authorization and credential separation preserved |
| raw NDNSD `deployments` dictionaries | typed catalog backed by signed definition and activation records | Read-only legacy conversion; never canonical invocation or readiness authority |
| mandatory `deploy().wait_until_active()` before legacy `submit` | direct `request(definition_or_ref, ...)` | Old sequence remains a supported optional prewarm and delegates to the same ensure-deployment operation |

## Warning Contract

Each deprecated surface emits at most one warning per process/call-site class,
including:

- exact deprecated symbol;
- exact replacement;
- semantic difference, if any;
- compatibility-window identifier;
- documentation link.

Warnings must not include secrets, payloads, tokens, or full deployment data.

## Exit Criteria

Compatibility removal requires all of:

1. all maintained repository examples and English/Chinese docs use canonical
   imports and signatures;
2. public compatibility usage evidence is below an approved threshold for the
   documented window;
3. one release has published migration guidance and warnings;
4. normalized behavior/security parity tests pass;
5. removal is proposed and audited in a separate feature.

Spec 116 implementation alone cannot satisfy criterion 5.

## Rollback

If the new facade fails acceptance, disable preferred exports/docs and retain
the existing APP role components and root compatibility manifest. Because the
new layer owns no independent state or wire protocol, rollback does not require
journal conversion or provider redeployment.
