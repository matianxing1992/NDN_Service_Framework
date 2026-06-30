#!/usr/bin/env python3
"""Runtime v1 profile campaign.

The default mode is a local contract campaign. It produces the same profile
surface a heavier MiniNDN campaign should consume: short context, long context,
provider failure, and high RTT.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from ndnsf_distributed_inference.runtime_v1 import (
    ModelManifestV1,
    ProviderProfileV1,
    adaptive_segment_size,
    build_local_llm_plan,
    load_provider_profiles,
    read_json,
    simulate_prefill_decode,
    write_json,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles", type=Path, default=Path(__file__).with_name("runtime_v1_minindn_profiles.json"))
    parser.add_argument("--model", type=Path, default=Path(__file__).with_name("llm_model_spec_qwen_tiny_proportional.json"))
    parser.add_argument("--providers", type=Path, default=Path(__file__).with_name("llm_provider_profiles_2_4_8.json"))
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    model = ModelManifestV1.from_dict(read_json(args.model))
    base_providers = load_provider_profiles(args.providers)
    rows = []
    for profile in read_json(args.profiles).get("profiles", []):
        providers = list(base_providers)
        if profile.get("failure") == "drop-largest-provider":
            providers = sorted(providers, key=lambda item: item.flops_tflops)[:-1]
        lease = build_local_llm_plan(
            model,
            providers,
            target_rps=float(profile.get("targetRps", 0)),
            context_class="long" if int(profile.get("contextTokens", 0)) > 4096 else "short",
            prefix_id="shared-prefix" if profile.get("cacheAware") else "",
        )
        provider = max(providers, key=lambda item: item.flops_tflops)
        generation = simulate_prefill_decode(
            request_id=str(profile["name"]),
            provider=provider,
            model=model,
            prompt_tokens=int(profile.get("contextTokens", 1024)),
            generated_tokens=32,
            microbatch=4,
        )
        segment = adaptive_segment_size(
            4 * 1024 * 1024,
            rtt_ms=float(profile.get("rttMs", 10)),
            bandwidth_mbps=float(profile.get("bandwidthMbps", 1000)),
        )
        rows.append({
            "profile": profile["name"],
            "planId": lease.plan_id,
            "providers": [item.provider for item in providers],
            "fallbackPlanIds": list(lease.fallback_plan_ids),
            "cacheProvider": lease.cache_placement.provider if lease.cache_placement else "",
            "cacheReason": lease.cache_placement.reason if lease.cache_placement else "",
            "timeToFirstTokenMs": generation.time_to_first_token_ms,
            "interTokenMs": generation.inter_token_ms,
            "adaptiveSegmentSize": segment,
        })
    write_json(args.out_dir / "runtime-v1-profile-campaign-summary.json", {"rows": rows})
    print(f"wrote {args.out_dir / 'runtime-v1-profile-campaign-summary.json'} profiles={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
