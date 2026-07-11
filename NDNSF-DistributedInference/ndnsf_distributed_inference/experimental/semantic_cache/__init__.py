"""Optional application-defined semantic response cache.

Applications own similarity decisions. The default DI package intentionally
does not expose these symbols. Exact Forward Cache remains a separate normal
provider-local optimization.
"""

from .implementation import (
    SemanticCacheAckHint,
    SemanticCacheDisposition,
    SemanticPatternMeta,
    SemanticPatternRank,
    SemanticServiceCacheEntry,
    SemanticServiceCacheKey,
    SemanticServiceCacheManager,
    choose_semantic_cache_provider,
    parse_semantic_cache_ack_hint,
    rank_semantic_patterns,
    semantic_cache_ack_fields,
    semantic_cache_token_saving_ratio,
)

__all__ = [
    "SemanticCacheAckHint",
    "SemanticCacheDisposition",
    "SemanticPatternMeta",
    "SemanticPatternRank",
    "SemanticServiceCacheEntry",
    "SemanticServiceCacheKey",
    "SemanticServiceCacheManager",
    "choose_semantic_cache_provider",
    "parse_semantic_cache_ack_hint",
    "rank_semantic_patterns",
    "semantic_cache_ack_fields",
    "semantic_cache_token_saving_ratio",
]
