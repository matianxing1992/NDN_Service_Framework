# DSH TPS window

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
The quota tooltip shows a single daily usage pace based on the longest reported
window. Short rolling windows use their full duration. Longer windows use elapsed
time at the quota snapshot, based on the reset timestamp and window length.
Monthly windows without a provider-supplied duration use 30 days. A longer
window with no valid reset timestamp has no pace reading.

This directory can be copied or cloned on its own. Use Node 22 or newer and
npm 10 or newer. From a clean copy, run `npm ci --ignore-scripts`, `npm test`,
and `npm pack --ignore-scripts`. Install the tarball with
`dsh plugin --profile web add /absolute/path/to/the.tgz`. Verify that
`dsh-tps-window` occurs once in the Web profile's `dsh.profile.bundles` and
restart the service through its existing owner.

The supported host is DSH `0.2.2-rc.1` with Cordis `4.1.0`. The Web loader
supplies React and the host supplies session events and authenticated Web routes.
The speed display needs no Codex CLI, credentials for quota providers, Codex
Audit plugin, or NDNSF checkout. Optional quota readings use the corresponding
account configuration or Codex CLI on the DSH host. Unconfigured providers
report unavailable and the speed display still works.

For a separate `open-design-2` model route, use `OPEN_DESIGN_2_API_KEY` in
the DSH model configuration. When the quota credential is missing, click
**连接 OpenDesign 额度** on the selected route. Open the displayed official
authorization link, approve with the corresponding account and verify the
displayed account and workspace before clicking **确认绑定**. If the browser
does not open, expand **手动打开** and copy the displayed authorization link.
If the browser
authorized the wrong account or workspace, cancel and authorize the correct
one. The plugin uses the device flow used by `vela login`. OpenDesign does not
document the device HTTP endpoints as a public API. A pending authorization
expires within ten minutes and is cancelled when the plugin is disabled.
Confirmed credentials are stored atomically in the DSH host's credential store
under a distinct reference for each route. Neither the runtime API key nor the
new control key is sent to the web UI.

After a DSH restart, the plugin reads each route's saved binding again. If
the Web connection or OpenDesign quota service is temporarily unavailable,
the quota display retries every 15 seconds using the same binding.

For manual setup, complete device authorization via the desktop app or
`vela login`. In DSH's private credential store (`$DSH_HOME/.credentials.yaml`,
under `refs`), add the device-issued
`OPEN_DESIGN_2_CONTROL_KEY` and the `OPEN_DESIGN_2_WORKSPACE_ID` of the
workspace that issued the inference key. The plugin queries that workspace's
wallet and its actual 5-hour or 7-day usage windows. Plans that return only a
7-day window display only that window. Signing into the website without device authorization does not
create a DSH credential, and the second route does not borrow the first route's
control key or use the desktop application's default profile.

Tests exercise the real native compact-stream expansion, Cordis Host notifications,
authenticated route, and real ModuleLoader client with a deterministic browser
fixture. No paid model calls or external provider requests are required.
