# Job 182500: clean-environment contract failure

Status: **FAILED before any User request**.

The immutable v67 campaign started three NFDs, established the three-node
routes, started the Controller and all three Repository and Provider processes,
then entered the new public-certificate exchange. All ranks exited at the same
shell line because the certificate temporary filename referenced
`SLURM_JOB_ID`, while `apptainer exec --cleanenv` intentionally did not pass
that scheduler variable into the container.

No Request gate opened, no model artifact was fetched, and no inference ran.
This is a deployment-launch contract defect, not a Qwen, Repository, cache or
distributed-execution result. The v67 campaign is closed and must not be
resubmitted. The linked repair uses the container-local shell PID (`$$`) for
the temporary filename and adds a regression forbidding Slurm-only variables
inside the clean container script.
