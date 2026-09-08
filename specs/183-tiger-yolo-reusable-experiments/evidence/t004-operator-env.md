# T004.l — isolated operator environment and interpreter dispatch

2026-09-07. IMPLEMENTED; actual operator dependency evidence plus component
launch checks. T004 and T007 remain incomplete. No Slurm allocation, SIF build,
model/GPU execution or large input download occurred.

Follow-up: [t004-submit-origin.md](t004-submit-origin.md) corrects sender submit
to use its invoking interpreter before receiver staging. Installation evidence
below remains valid; the original submit-selection description is historical.

## Actual environment

Tiger login Python: 3.9.18. Created the independent prefix
`/project/tma1/ndnsf-di/operator-envs/py39-3b4f62bc` with the maintained
`tools/spec183_operator_env.py` using venv --copies and binary-only pip installation.
The global interpreter was not modified. Requirements digest:
`sha256:3b4f62bcad8e182c7402f9068dce192afd3283be0b9fc5ec2d0e597aa7a9b755`.

Installed/import-verified versions: numpy 1.24.4, jsonschema 4.21.1, attrs 23.2.0,
jsonschema-specifications 2023.12.1, referencing 0.33.0, rpds-py 0.18.0.
Imports came from this prefix's lib64/python3.9/site-packages, with sys.prefix
equal to the declared environment. Platform: EL9, glibc 2.34. This is login-node
evidence; compute-node loading remains an actual-allocation check.

Installation returned VERIFIED/OPERATOR_DEPENDENCIES_ONLY, reused=false. A second
invocation revalidated the same prefix with reused=true, without reinstalling.
The local /usr/bin/python3 separately passed the same maintained requirements
validator, including its Python <3.9 conditional pins. No duplicate local venv
was installed because the existing local environment already passed.

## Paths and provenance

Local raw root: `Experiments/TigerCluster/results/spec183-operator-env-20260907/`.
Files: install-receipt.json, reuse-receipt.json, local-preflight.txt; stderr files
are empty. Durable remote installation log and operator-environment.json are in
the prefix above.

The minimal five-file bootstrap archive was copied to
`/project/tma1/ndnsf-di/candidates/spec183-operator-bootstrap-20260907/`.
Local and remote archive SHA-256 both:
`1b70662b6835f350b458f3af2be662c69cdc9bee655cf8a805d0e751b7f012f0`.
This directory contains setup tools, not a qualified YOLO candidate or a SIF.
All maintained source remains under Experiments/TigerCluster.

## Wiring and bounded tests

runtime.operatorPython is a physical locator excluded from normalized behavior;
the raw profile digest still binds it. The actual profile selects the new prefix.
Submission passes the interpreter as a sixth bound batch argument; the shell
never reads mutable profile data to select an executable. srun receives the same
path. Local CPU/plain collect retain the invoking host's interpreter, and each
entered frozen CLI verifies actual dependency pins. Cluster submit/reconcile
select the configured interpreter after profile digest verification.

| Artifact | Result | Scope |
| --- | --- | --- |
| focused.xml | 104 passed, 18.91 s | profile/schema/normalization, bootstrap, real shell wrapper fixture, CLI and prior terminal boundaries |
| final-boundaries.xml | 78 passed, 17.44 s | final host-vs-cluster interpreter selection, pin rejection before execution, sbatch/srun argument agreement and affected CLI/journal paths |

Groups overlap; do not sum them into a unique-test claim. Shell and journal tests
use explicit fake interpreters/scheduler components; the installation/import
evidence above is separate and real. No native, SIF or GPU qualification follows.

Next: portable prerequisite/file transport, registered negative runner, actual
compute loading and the formal runtime gates. Base-SIF read integrity remains
unresolved; no new candidate render was attempted in this checkpoint.
