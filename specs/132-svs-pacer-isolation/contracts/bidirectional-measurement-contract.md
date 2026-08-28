# Bidirectional Measurement Contract

1. Each cell has two equal peers; both publish and subscribe concurrently.
2. The target rate is per peer; aggregate target is exactly twice that value.
3. Face processing runs on an I/O thread in each process.
4. The application main thread calls `publish()` for the synchronous subject or
   `publishAsync()` for the asynchronous subject directly.
5. The harness contains no publication adapter queue, eventfd, Asio post, or
   scheduler-generated offered load.
6. Absolute deadlines avoid sleep drift, but a slow API call is not hidden.
7. No new publication call begins after the fixed measurement boundary.
8. API entry, successful return, and opposite-peer delivery are different
   events and denominators.
9. A-to-B and B-to-A pass independently; a cell passes only if both pass.
10. Formal order is five synchronous cells followed by five asynchronous cells.
11. Every cell has one attempt and one terminal receipt; no formal retry exists.
12. Subject crash is retained as negative evidence; infrastructure failure is
    labeled separately.
13. Frozen Spec 131 output and the failed adapter diagnostic are never inputs to
    the new formal aggregate.

