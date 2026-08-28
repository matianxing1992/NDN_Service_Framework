# Implementation Plan: Predictive Half-window Confirmation

Replace the generic horizon helper's 25% reserve with a direct half-capacity
rule:

```text
capacity = min(lookahead, active aggregate window)
horizon = capacity == 0 ? 0 : max(1, floor(capacity / 2))
```

This is the smallest follow-up to Spec 155: it changes no controller, retry
order, public API, wire format, or application policy. It shortens how far
exact future names can lead production, leaving half the active capacity for
callback jitter and retry work. After red/green tests and the unchanged
pre-formal gates, run the same immutable six-rate MiniNDN matrix once.

## Constitution Check

Ownership remains generic Core scheduling; UAV APP retains only its thread
lifecycle fix. Security/API/wire are unchanged, negative evidence is preserved,
and MiniNDN remains the final verifier.

## Rollback

Restore the previous helper arithmetic only in a later successor Spec; never
rewrite this campaign.
