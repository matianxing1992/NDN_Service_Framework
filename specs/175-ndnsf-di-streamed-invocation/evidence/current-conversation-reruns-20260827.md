# Spec175 M11--M14 seeded real-Provider checkpoints

Date: 2026-08-27  
Subject: current working tree, tiny causal ONNX CPU fixture, four independent
MiniNDN Providers, real NFD/SVS/ABE, admission disabled, `seed=1750001`.

Each case completed with exit code 0 and a structured result containing the
same seed and campaign identity (`spec175-<case>-1750001`). The result files
remain in their temporary run directories; only hashes and bounded metrics are
retained here.

| Case | Result SHA-256 | User-log SHA-256 | Key evidence |
|---|---|---|---|
| M11 | `sha256:37b8711124a042b57ec35831263f67641d2d7ef13c8bda08dc4973aa85eb1e21` | `sha256:962d567c982b985aa58004900bb1083f2632dc49233ab74366311f8d4975c7ef` | 2 fresh Requests, 2 generations, 4 conversation hits, epochs 1→2, 0 state bytes on NDN, 0 request-local leaks |
| M12 | `sha256:031be9b3ad428d98da6109a8459ef57bdf869e0c88cdd6c707393eef0e25f2cb` | `sha256:aded32e84c028a4940ce609200b16255f17b55b983f04a8894d1009ca865e8f8` | 3 isolated conversations, 6 fresh Requests, 12 hits, 24 HOST pauses, 12 HOST→GPU prefetches, 0 leaks |
| M13 | `sha256:aade3919f238805377fe1051634bcadf84d6d45b41924caa87cbbfb09307395f` | `sha256:1406cb306557c8ac6babcae74f513768c402973a587ce2e97e00520538ee5438` | 4 Provider restarts, 1 pre-network invalid-checkpoint rejection, 1 authorized full-prefill fallback, 0 state bytes on NDN |
| M14 | `sha256:e85413c90fa86f9ee3c350c45c7f3fb7b62f2dd3d4fdc105604c0f3399170eac` | `sha256:42e31da5b98b59823eb9670ec47f639307908b65b560df6be496142a6c7cc457` | 3 fresh child Requests, 4 HOST pauses, 4 prefetches, 1 Provider-side cancelled prefetch, 4 hits, sibling completion |

The cases prove the current host/CPU real Provider path, not CUDA, SIF,
TigerCluster, Qwen3.6-27B, or repeated qualification. M14 cancellation is an
explicit Provider-side test fault; the User-side `cancel()` operation remains
local and no remote cancellation Interest is claimed. T020 still requires a
new clean source seal and the formal repeated G0--G2/G3 sequence.
