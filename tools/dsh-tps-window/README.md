# DSH TPS window — local 0.5.2

The `60s 合计` pill on a top-level session reports that session plus all local
descendant agents. The `60s 自身` pill on a child reports only that child's own
output, excluding its parent, siblings, descendants and inherited fork history.
All contributions use the same rolling wall-clock window; parallel request
durations are not added together. No output remaining in the window means zero.

Collection runs in the Host through public `agent/assistant-stream` and
`session/event` notifications, even when a child view is closed. Each attempt is
identified by its generating session and attempt ID. Final native compact streams
and `usage.outputTokens` replace provisional samples, never add a second copy.
Failed/abandoned attempts are discarded; committed interrupted output remains.
Live samples estimate tokens from output characters; settled usage calibrates
the owning attempt. Where no stream exists, usage is attributed to its settlement
time. Recent own-history events seed the window at startup via a bounded live
Session observation; original fork events are excluded using `Session.isOwnSeq`.

The window has a two-second minimum denominator and at most sixty seconds of
observed output, including pauses. Root totals therefore need not equal the sum
of child rates computed over different individual observed spans. Hover text
explains scope and whether unsettled estimates remain. Native whole-session
statistics are independent; this patch owns the labelled sixty-second pill.

`/tps-window/speed` requires the existing DSH authenticated connection. Browser
polling owns one cancellable request per view and releases its timer on unsubscribe.
The Host bounds retained sessions, steps, buckets and startup scanning. Overflow
or invalid stream data reports unavailable instead of a partial total or false zero.
Only timing/counts are retained; no generated text is persisted by this plugin.
Provider quota controls and their existing credentials/cache paths are retained.

Verification: run `node test/run.js` using the installed DSH Node runtime.
Tests exercise the real native compact-stream expansion, Cordis Host notifications,
authenticated route, and real ModuleLoader client with a deterministic browser
fixture. No paid model calls or external provider requests are required.
