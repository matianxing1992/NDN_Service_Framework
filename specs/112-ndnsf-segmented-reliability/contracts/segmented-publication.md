# Contract: Existing SVS Segmented Publication

## Scope

This contract closes email defects 1 and 2 on the existing ndn-svs publication
path. It does not add a public API, wire namespace, response policy, reference
format, or receipt protocol.

## Producer Invariants

1. Segment safety is decided from the final signed inner Data embedded in the
   final signed outer Data.
2. Every final encoded packet is at most `MAX_NDN_PACKET_SIZE` (8800 B in the
   current dependency).
3. A failed asynchronous preparation cannot consume a visible sequence, create a
   gap, or permit a later sequence to advance past unreadable state. Any internal
   tentative reservation is either committed after safe preparation or rolled
   back by the same ordering owner.
4. All packets required for one logical publication are prepared and readable
   before its sequence/notification state is advertised or its historical
   sequence return becomes caller-visible.
5. If encoding, signing, storage, or emission fails, incomplete prepared state is
   removed and the failed publication is not advertised as readable.
6. Exceptions thrown in posted/asynchronous callbacks are caught at the owning
   event-loop boundary and cannot terminate the Provider.
7. Existing `publish(...)` and `publishAsync(...)` public signatures and return
   types remain unchanged.

## Receiver Invariants

1. Segments are validated with the existing configured validator.
2. Missing, malformed, duplicate, conflicting, and validation-failed segment
   paths terminate and reclaim their subscription/fetch state.
3. A complete response is delivered once and is byte-identical to the published
   application payload.
4. Late segment events cannot revive a timed-out request or poison later fetches.

## Diagnostic Integration

- The MiniNDN Provider/User roles set
  `NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1` only for the declared Spec 112
  cell so that 6.5-KB, 8-KB, and 16-KB responses reach this contract.
- The result records the effective flag and absence of a large-reference event.
- This flag does not define a product transport mode and does not authorize any
  change to current automatic externalization behavior.

## Prohibited Behavior

- Treating raw application content size as the final packet budget.
- Advertising a sequence before every packet is readable.
- Empty validation-failure handling or callbacks that reference expired stack
  state.
- Catching an oversize exception after inconsistent sequence state is visible.
- Adding `publishChecked`, a new remote failure protocol, or a large-object
  redesign under Spec 112.
