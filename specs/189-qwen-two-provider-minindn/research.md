# Research: Qwen Two-Provider Full Path

## Decision: prepare-time Repo publication

The model is split and published once during `prepare`; requests carry only a reference. This follows the user-facing lifecycle and removes request-time publication of a 1.5-GB initializer. It also lets multiple requests reuse the same immutable manifest while retaining per-request authorization and runner state.

## Decision: Selection gates materialization

Providers do not prefetch layer bytes before authenticated Selection. The manifest and digest are safe summaries; layer fetch and runner creation are placement-bound effects.

## Decision: C++ owns native behavior

The requester, provider, assembly worker and output oracle are native C++ targets. Python launches MiniNDN, samples host resources and records evidence only.

## Decision: two stages first

The initial candidate uses Qwen3-0.6B's 28 layers split into ranges `0..14` and `14..28`. Any split change invalidates the manifest, stage artifacts, profile and evidence.

## Known boundary

Prior Qwen runs stopped around 6.7 GiB process RSS / 1.65 GiB MemAvailable before Selection/provider execution. That evidence is a resource boundary, not a protocol result. Spec189 must determine whether prepare-time Repo publication and placement-bound assembly remove that peak; it must not hide the boundary with a smaller model or preloaded runner.
