# Native Tiger deployment

## Select a physical allocation

Use the explicit SSH account in the skill and collect a current scheduler snapshot before submitting.

```bash
sinfo -N -o '%N %t %c %m %G'
scontrol show node
scontrol show reservation
squeue -u "$(id -un)"
```

- Estimate schedulable memory from `RealMemory - AllocMem - MemSpecLimit`. Slurm
  `FreeMem` reports OS observations. Use unallocated GRES to identify eligible
  nodes. Inside the allocation, bind the actual GPU UUID and check current signed
  free memory and runtime capability.
- Treat `Resources` and `Priority` as scheduler boundaries. `squeue --start`
  and `sbatch --test-only` are estimates. Keep the existing owner and job
  reference during a wait. Use CPU allocations for independent CPU reference,
  preparation or authorized CPU inference while GPUs are scarce. A physical-only
  task keeps its models, builds and tests on Tiger.
- On node failure, verify current state and all eligible pairs. Retarget only
  never-allocated duplicates after checking their exact IDs, owner and state.
  Preserve completed results. For performance comparisons, use matched hardware,
  resource limits, image and workload. Keep samples from materially different
  pairs in their own series.

## Launch and close one owned deployment

Freeze source including relevant dirty changes, native SDK and application SIF,
host control package, model weights and tokenizer, role mapping, configuration and
oracle. Inspect the installed CLI and schema for the exact candidate. For a
normalized Slurm deployment, the maintained operations are:

```bash
ndnsf-di cluster check --config "$deployment"
ndnsf-di cluster up --config "$deployment" --timeout "$run_budget" --timing
ndnsf-di cluster status --config "$deployment" --timeout "$status_budget"
```

Use `cluster prepare` only for the preparation workflow your configuration
declares. Use `cluster down` for required closeout of that same deployment.
One owner controls the allocation and its writers. If submit or status loses
its response, reconcile the original job, boot and config digest before retrying.
For an attached owner running as a Slurm job, size its own `--time` to cover
its CLI wait and closeout. Check both jobs' actual time limits after submission.
The queue delay counts against the running owner's wall time. Its SIGTERM
handler can cancel the child when the owner expires. A normal account may be
unable to extend an already running owner. If replacement is necessary, verify
that the original deployment never acquired nodes before releasing a successor,
and repoint the acceptance dependencies to the new verified receipt chain.
After cancelling a held job, use the verified numeric ID for accounting if the
name/time query returns no rows. Match its owner, boot-qualified job name and
submission time. Confirm from duplicate-aware accounting that it never started
or received nodes before recording pre-allocation cleanup.
When Slurm has evicted an old completed prerequisite, verify its exact
`sacct` status, exit code and artifact identity before submitting. Use scheduler
dependencies for still-active jobs and check the completed receipts again in
the consuming job.

For a physical two-node PP run, each node runs one owned NFD and one Provider.
Configure each actor's local Unix socket, bidirectional TCP faces, exact
producer/service prefixes and the intended forwarding strategy. Verify effective
FIB/strategy and a cross-node Interest followed by admitted producer-signed Data.
Preserve identities, authorization, content names and state lineage through the
ordinary native entry points. Run C++ applications and probes in the container.
Host Python can prepare inputs and orchestrate the installed native runtime.

`source-cache` verifies and consumes sealed source files. Record this scope
separately from publishing to and retrieving exact Data from a Repo. For
`stored`, verify actual committed root/object readback with the original
identity and the production read path. Keep frozen inputs read-only and writable
state/cache/scratch distinct. Verify actual host/container mounts, particularly
the container's `/scratch`.

Before qualifying, require terminal Slurm success, independently verified
results and the original nodes' nonforced host audits for owned processes and
cgroups. Keep the evidence and mark pending closeout if either original audit
is unavailable. Review cleanup for both terminal jobs and still-pending
consumers before retiring any shared path.
