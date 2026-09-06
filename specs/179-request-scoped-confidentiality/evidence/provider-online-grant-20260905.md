# Controller-authorized Provider online grant

Status: T021 runtime/example fixes,28/28 launcher checks and rebuilt native gates pass;
final18-scenario MiniNDN acceptance pending.

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
