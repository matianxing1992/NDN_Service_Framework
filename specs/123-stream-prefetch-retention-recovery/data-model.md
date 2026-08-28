# Data Model: Stream Prefetch Retention Recovery

## Payload lifecycle

- `ReservedFuture`: Mapping exists and the cursor has never been materialized.
- `Retained`: signed Data has been materialized and is available in the live retention store.
- `Evicted`: cursor was materialized but signed Data is no longer in live retention.

Only `ReservedFuture` is eligible for Provider pending-Interest admission.

## Scheduled payload Interest

- cursor and semantic Data name
- whether it was ahead of the best known produced observation when emitted
- authoritative bounded Interest lifetime
- retry attempt and deadline

## UAV retention policy

- target duration in milliseconds
- nominal frame rate
- data plus parity items per frame
- computed retained-item bound
- hard maximum retained items/bytes

## Ordered access-unit assembly

- session epoch and frame identity
- exact capture/PTS binding
- expected source-segment count
- received segments and bytes
- first/last arrival time
- exactly one active frame at a time because Core delivery is cursor ordered
- terminal state: complete, incomplete-next-frame-drop, conflict-drop, or session-drop
