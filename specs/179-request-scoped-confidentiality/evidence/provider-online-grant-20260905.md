# Controller-authorized Provider online grant

Status: T021 implementation and network validation pending.

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
