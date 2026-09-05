# Controller-authorized Provider online grant

Status: T021 example/launcher changes and27/27 component tests pass; native/network pending.

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
