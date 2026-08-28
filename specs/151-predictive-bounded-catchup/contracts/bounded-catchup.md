# Bounded Catch-up Contract

For every predictive scheduling decision:

```text
1 <= futureCursorHorizon
futureCursorHorizon <= decision.lookahead
futureCursorHorizon <= aggregateInterestLimit
inFlight + processing + scheduled <= aggregateInterestLimit
```

Selection order:

```text
valid pending source retries
  before
new sequential/future cursors
```

The horizon grants names that may be requested. It does not bypass the
aggregate budget, create a second request path, change wire names, or imply
that every future Interest will be useful.

MiniNDN contract:

```text
LD_LIBRARY_PATH=<repo>/build:<repo>/.local-boost171/lib
```

Both UAV processes must resolve the frozen build Core.
