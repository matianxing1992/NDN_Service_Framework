# Job 182504: nonessential certificate-prefetch tool missing

Job 182504 was the single formal campaign for v72.  NFD, routes, three Repo
nodes, automatic planning, and all three Providers started.  It failed before
the User request, model fetch, or inference because the newly added shell
certificate matrix invoked `ndnpeek`, which is not present in the sealed SIF.

The failure also exposed a design error: eagerly fetching every certificate is
an artificial global startup barrier.  NDNSF already publishes each runtime's
public certificate, and `MessageValidator` has a network certificate fetcher.
The v73 repair therefore restores request-first, data-driven behavior:
certificates are fetched from the Data KeyLocator only when ACK, Selection,
status, or stage Data is validated.  The analyzer requires configured-validator
success and rejects any final validation failure.  Job 182504 and its campaign
identity are closed and must not be retried.
