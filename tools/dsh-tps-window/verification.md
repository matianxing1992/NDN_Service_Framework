# 0.5.2 verification (includes 0.5.1 rollout)

Scope: the DSH plugin `dsh-tps-window`, not LLM Vitals or the Codex extension.
Original installed package: 0.5.0. The earlier LLM Vitals investigation made no
changes to its installed files. An optional upstream checkout was read only.

Confirmed against actual installed DSH headers/source: transient output uses
`agent/assistant-stream`; the browser synthesizes `assistant/live-chunk` and
publishes final settlement as `settle-assistant`, not ordinary append. Original
plugin subscribed only to the displayed session and ignored settlement changes.
Replaying its actual ModuleLoader bundle produced no parent reading when the
child streamed; child retained 64 estimated tokens after actual 200-token usage
settled. This is a reproduction of missing aggregation/calibration, not a live
paid generation or evidence that native whole-session statistics are changed.

New collector attaches global public notifications and keys attempts by actual
generating session. Ownership uses durable parentSession plus live registry
isOwnedBy, not arbitrary fork ancestry or a caller's scoped parent. Startup uses
bounded live-preferred observations, filters Session.isOwnSeq, releases all leases,
and expands the authoritative compact stream via the installed native DSH helper.
Root sums descendant output; a child measures only itself, including when its own
child works. Unique settlement seq/attempt identity prevents duplicate charging.

`node test/run.js` on the DSH Node 24 runtime exercises 9 checks: aggregation and
common wall denominator, idle parent/retired child, live+settlement+replay dedup,
fork inheritance, abandoned retries, malformed stream reporting, false lineage,
real ModuleLoader frontend polling/labels/cleanup, and real Cordis Host event/route
integration with authentication. No model requests or external quota reads.
JavaScript syntax and git whitespace checks are separate from runtime activation.

Existing quota calculation and provider request code were retained. The native
whole-session activity pill remains independently owned by DSH; the updated plugin
owns the explicitly labelled sixty-second aggregate/self pill.

KVCache@8027f6c6b5e9c339e809da24a85661c183cbb421 contains no corresponding plugin
implementation. No NDNSF source, Waf configuration or running goal is changed by
this task. Installation/activation results are recorded after artifact verification.

Installed 0.5.1 using the official DSH CLI. Offline installation initially lacked
the pinned native helper's registry metadata; the same exact-version dependency
was then obtained through normal package management. No toolchain upgrade was
performed. The Host's and installed profile's expandAssistantStream functions
were compared and are identical. All 7 packaged files match maintained source
and installed bytes; archive SHA256 is
`6d2af5f6d27dfb544cb1aaedca397bc61a45bd3df77b03cd8d60eb27502f4893`.
The archive is retained at
`~/.dsh/profiles/web/plugin-packages/dsh-tps-window-0.5.1.tgz`.

Activation initially returned applied while the speed route still returned the
old unknown-path response. Investigation identified a separate local 0.5.0
override at profiles/web/plugins/dsh-tps-window shadowing node_modules. That
directory was preserved at ~/.dsh/backups/tps-window-0.5.0-before-0.5.1 and replaced
with a relative link to ../node_modules/dsh-tps-window. Future package updates
therefore follow one installed authority instead of the stale source copy.

Official pluginManager/setPluginEnabled off/on activated the actual new module.
Authenticated GET /tps-window/speed returned status ok, scope family and zero
for the observed empty window. The original native goal ID/revision/phase/
activation were independently compared before/after and were unchanged. No DSH
service restart, injected model request, internal cache mutation or other-plugin
change was used. Positive multi-agent accounting is proven by deterministic
collector and actual Cordis event/route fixtures; a live paid multi-agent
generation and pixel-level browser rendering were not run. Refreshing the DSH
page loads the new aggregate/self labels. Native whole-session statistics are
not claimed repaired by this plugin patch. Temporary build/reproduction inputs
are retired after verification.

Final 0.5.2 also captures all live parent edges before the first startup await,
retains those identities during seeding, and limits observation with a five-second
abort signal. A regression disposes a child during its history observation and
still proves its output belongs in the parent total and only itself in the child
view. Final **10 tests pass, zero failures/skips**, using Node 24.

Final archive SHA256:
`252d22572bf1beb569319e71bbf07ffb6df6a7db6d313dd78a2db9e1aae865c4`.
All 7 files match source/archive/installed package; the profile points to the
durable plugin-packages/dsh-tps-window-0.5.2.tgz, and the local override follows
the installed package. Official disable/enable loaded it with original goal
identity/revision/phase/activation unchanged. Authenticated live readings then
reported family/members=2 for the main conversation (about 269.2 tok/s) and
self/members=1 for its KV child (about 228.4 tok/s). These were instantaneous
stream-calibrated estimates, not a provider performance benchmark. A preliminary
subagents/listDescendants RPC lookup was not exposed (404); child identity was
instead read from the maintained parent catalog cache without changing it.
The speed API itself returned status ok in both views. No extra model call or
service restart was used. No LLM Vitals/Codex extension file was changed.
