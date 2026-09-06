# Controller-authorized Provider online grant

Status: T021 local PASS. Runtime/example fixes,28/28 launcher checks,
183/183 unit cases,72/72 integration cases and18/18 MiniNDN scenarios pass.

The authoritative final cohort is `campaign-ack-final/` at clean NDNSF
`994018ac5c03f5f8aa9cf81614334f0713449d6e`. Earlier pending statements below are
historical checkpoints, superseded by the final acceptance section. NAC source
remains `b3b43c8` on local `Experimental` (documentation HEAD `85547eb`).

User clarification: authorization means Controller permission to use services
as a User and to offer services as a Provider. Existing16-case MiniNDN evidence
at de1eb508 covers User online grant and both-role revocation, not Provider
first-grant execution. Do not generalize it to that missing case.

Pre-implementation audit: PASS for bounded T021 repair. Controller's generic
grant already accepts `/SERVICE` attributes; its component test covers policy
assignment. The example timer always constructs `/PERMISSION`, and Provider
example has no explicit renewal timer. Fix example role selection and mirror
the existing User renewal mechanism, with two real Provider grant scenarios.
No runtime authority redesign, wire/API removal or new framework polling is
needed. Keep User as the default role. Invalid role input must fail explicitly.

Required evidence: initial Provider pending/no service; a Controller `/SERVICE`
grant; post-grant permission renewal and one target DKEY refresh; actual
Provider response and successful requests selecting it; zero unaffected-role
DKEY refreshes and control failures. The late variant additionally requires an
observed final permission timeout before grant and a subsequent renewal before
service. Retain all failures; test malformed/missing/early evidence negatively.

Workflow: existing Context/CodeGraph/Spec Kit/GSD gates continue from T020;
provider-specific CodeGraph queries and repository source confirmed the gap.
ARS remains inapplicable to this implementation regression.

Implemented Controller `--grant-additional-role=user|provider` (default User)
and Provider `NDNSF_PERMISSION_REFETCH_AFTER_MS`, matching the User example.
New scenarios are `provider-grant-only-advance` and
`provider-grant-after-permission-exhaustion`; both keep Provider/A as control,
target User/B exclusively at Provider/B, and renew User/B's provider table after
the Provider grant. The late scenario isolates UDP loss in Provider/B's own
namespace until its observed final permission timeout, then removes the rule.

Evidence root remains `results/spec179-nac-compatibility-20260905/`.
`provider-grant-evaluator-red.log`:11 failures before the evaluator exists;
`provider-grant-evaluator-green.log`:11 pass; combined launcher/guard/evaluator
`provider-grant-launcher-final.log`:27 pass. The initial combined failure only
matched the old User-only guard-error wording; the final test covers both host
role names and ensures neither can reach packet-filter mutation.

## First network findings (retained negative)

At1823364c, both new Provider cases complete but fail their gates (CLI4,
driver1); original User grant control passes. `provider-campaign-first/` shows:

- Benchmark open/closed-loop paths ignore `--known-provider-ids`. Provider/A
  produces successes intended for B, even before B's authorization.
- User treats the new Provider route as a new ABE attribute and unnecessarily
  refreshes its unchanged `/PERMISSION/HELLO` key. Controller service grants
  must be distinguished from provider-table route changes.
- Provider's existing status-advance handler calls
  `refreshProviderPermissionsAfterAdvance`; this can precede the App timer.
  It is valid automatic discovery, not unauthorized behavior. The revised
  evaluator observes actual permission fetches and requires App renewal to
  remain idempotent instead of forcing it to be the first fetch.
- Normal control31/32 and target31/32 include a version-transition timeout.
  The new scenario uses the existing User grant probe's250ms status cadence
  and still retains every failure. ControllerVersion transitions can cancel
  in-flight requests; this test does not promise uninterrupted traffic under
  arbitrary request/refresh timing.

Repairs use existing explicit-provider RequestService overloads for benchmark
calls, including custom selection adaptation, and compare User authorized
service sets when deciding DKEY refresh. The Controller integration test adds
a Provider route to an already authorized service and requires zero additional
User fetches. No NAC source or protocol format changes are needed.

Repaired native build passes in4m27.010s at-j2 (`provider-grant-runtime-build.log`);
six target closures select the unchanged NAC925e983b library. Unit182/182,
11971 assertions and integration72/72,1281 assertions pass, including the
route-only grant regression (`provider-runtime-{unit,integration}.log`). Final
launcher/evaluator/namespace checks pass28/28 (`provider-grant-launcher-complete.log`).
The positive automatic-renewal fixture explicitly permits status discovery
before the later idempotent App timer. Full18-scenario acceptance remains pending.

## Explicit Provider ACK boundary

The next cohort at925ec3a9 (`provider-campaign-final/`, superseded despite its
directory name) exposes an additional runtime omission. Both Provider cases
pass all checks except exact Provider selection:17/17 and22/22 post-renewal
requests succeed, but some select A. The benchmark now passes B correctly;
`handleRequestAckByName` checks Controller permission without checking the
pending call's explicit candidate set. A is authorized generally, but is not
the caller's requested Provider. Stop the remaining campaign (driver143) and
retain both failures; the in-flight User control also finishes successfully.

Reject out-of-set ACKs centrally before status/selection effects, balance
tracked decrypt completion, and preserve empty-list discovery. The new
`ExplicitProviderRequestRejectsOtherAuthorizedProviderAcks` native case covers
FirstResponding, RandomSelection and AllSelected. No authority wire format or
class layout changes. Final full native/network acceptance remains pending.

ACK-boundary build passes in8m2.858s at-j2; six target dependency closures pass.
`provider-ack-unit.log` passes183/183 cases (11998 assertions), including all
three selection strategies; `provider-ack-integration.log` passes72/72 cases
(1281 assertions). Python28/28 remains unchanged. A new complete18-case
network cohort is required; neither earlier Provider cohort is acceptance.

## Final acceptance

All18 scenarios in `campaign-ack-final/` complete with `gatePassed=true`,
`networkEvidence=true`, no launcher error and CLI exit0; the driver exit0 is
retained in `campaign-ack-final.exit`. `final-network-verification.log` verifies
188/188 scenario assertions, both dedicated User grant gates, clean994018ac
for every manifest and33 identical artifact hashes against the actual files.
The NAC library remains SHA256
`925e983ba167ca158ce0fc2e8dd015fd2c9afc2971cf0ccdd5b88c66637223b0`.

| Scenario | Final evidence |
|---|---|
| Provider first grant |13/13 assertions; target17/17, control32/32 successful requests |
| Provider grant after permission exhaustion |14/14; target22/22, control65/65; timeout < grant < actual renewal < service |
| User first grant |Dedicated gate PASS; target10/10, control24/24; one target DKEY fetch, zero unaffected fetches |
| User grant after permission exhaustion |Dedicated gate PASS; target21/21, control60/60; one target DKEY fetch, zero unaffected fetches |
| Revocation rotation failure and retry |14/14 |
| In-flight revocation |12/12 |
| User identity revocation |12/12 |
| Provider identity revocation/restart |14/14 |
| Service-scoped revocation with unaffected control |11/11 |
| Offline rejoin across epochs |8/8 |
| Controller/cache/Provider status retrieval |14/14 |
| Controller unavailable and permission expiry |7/7 |
| Large response invalidation |14/14 |
| Targeted refill invalidation |14/14 |
| Stream invalidation |6/6 |
| Hintless scheduled refresh |6/6 |
| Controller restart |15/15 |
| Selection/response tampering and replay |14/14 |

The planned Provider restart and Controller outage have role exit-2, with
recovery/expiry checked explicitly. All other role exits are0. No failed target
or control rows are omitted. Provider grants use `/SERVICE/HELLO`; User grants
use `/PERMISSION/HELLO`. App provider lists and runtime ACK admission now enforce
the same explicit selection constraint. A new Provider route does not refresh
an already authorized User's unchanged ABE service attribute.

This establishes the listed Controller authorization and revocation cases on
the pinned local build/configuration. It is not proof of every possible timing,
fault or deployment. Version transitions may cancel in-flight requests; Apps
own retries and permission discovery, with Provider status-triggered renewal
also supported. Ordinary source calls remain supported; NAC changed public
layouts, so clean matching rebuilds are mandatory for other applications and
language extensions. Python bindings, third-party deployments and TigerCluster
are not qualified by these C++/MiniNDN results. Next delivery item is T014,
upstream publication and a matched dependency release; nothing was pushed.

Workflow closure: Context Mode repository authority was refreshed and checked;
CodeGraph verified NDNSF sources and synced changes (NAC has no index); Spec Kit
records requirements, findings and execution; GSD retains the diagnosis state.
ARS is not applicable to this implementation regression. Compiler/tooling and
network failures remain in `docs/failure-log.md` and the ignored evidence root.
