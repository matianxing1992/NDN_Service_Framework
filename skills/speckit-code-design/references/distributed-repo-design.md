# NDNSF DistributedRepo Design Review

Use this reference when a change affects `NDNSF-DistributedRepo` storage,
publication, retrieval, receipt/status, reuse, or a caller such as NDNSF-DI.
It is a design checklist, not an implementation prescription. Confirm each
behavior against current code and user requirements; label unsupported behavior
as planned or unknown instead of implying it already works.

## Keep the Two Modes Distinct

Describe each mode's owner, lifetime, call path, acknowledgement, placement,
replication, and cleanup separately:

- **In-app mode** is a library used by an application. The application invokes
  repository operations in its own process/lifecycle. Do not imply that this
  mode owns replica placement unless the implementation explicitly does so.
- **Persistent mode** is a long-running repository service on a node. State
  which node owns the stored copy and replication work. After receiving
  provider acknowledgements, the User chooses the storage location and desired
  replicas; the design must state what the acknowledgement promises and when
  placement/replication becomes observable.

Do not treat an acknowledgement as proof that every requested replica already
exists. Define the state transitions for accepted, stored, replicated,
partially replicated, failed, and unavailable outcomes, as applicable.

## Reusing Stored Objects

Both modes must be considered for using a previously stored object as an input
to a later service invocation. Define how a caller identifies and references
that object, how the receiver resolves it, and what happens if the reference is
stale, unauthorized, unavailable, or only partially replicated. Keep object
identity/content version separate from its current placement and availability.

Trace representative NDNSF-DI paths, including reuse of prepared models,
intermediate artifacts, and prior request outputs where applicable. Verify
which paths already use DistributedRepo and which still transfer inline or
through another store; do not infer adoption from a type name or documentation
alone.

## Receipt and Current Status

Specify a receipt returned after a store operation. At minimum, decide how it
identifies the stored object, reports the selected storage/replica locations,
and exposes the forwarding hint(s) needed to retrieve it. Define whether the
receipt is durable evidence of the accepted write, a snapshot of placement, or
both; do not conflate that evidence with live availability.

If callers need current status, define a query that can report separately:

- whether each recorded node is currently reachable;
- whether the identified object is currently present and readable there;
- when and by whom that status was observed, and how stale results are exposed;
- how deleted, replaced, moved, or no-longer-authorized objects are represented.

Treat node liveness as evidence about the node, not proof that a particular
object is available. Define partial results and query failures.

## Retrieval and Forwarding-Hint Retry

For a timed-out retrieval Interest, define how the next eligible forwarding
hint is selected. The retry flow should avoid sending repeated attempts to the
same node while another untried eligible location remains. Specify the scope
and lifetime of the attempted-node set, handling of duplicate hints, exhausted
candidates, late Data from an earlier attempt, and the overall request deadline.
Do not claim that a new hint identifies a distinct node unless the code
normalizes and checks node identity.

## Required Design Evidence

For affected behavior, include a compact mode-by-operation matrix covering:
store/publish, acknowledgement, placement/replication, receipt creation,
receipt status query, reuse as a later input, retrieval, timeout/failover, and
cleanup. Omit only operations proven out of scope. Bind each row to the real
public entrypoint and its implementation/callers.

Include failure scenarios for at least the applicable cases: no provider
acknowledges; storage succeeds but replication is partial; receipt is stale;
node is reachable but object is missing; retrieval times out at one location;
all locations fail; the object changes version; and a caller attempts reuse
without access. Add domain-specific DI cases discovered from the current
callers. Every scenario needs an observable result and an independent
acceptance oracle in the design and task list.
