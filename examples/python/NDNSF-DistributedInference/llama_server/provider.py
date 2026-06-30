#!/usr/bin/env python3
"""NDNSF-DI provider that proxies requests to a local llama-server."""

from __future__ import annotations

import argparse
import json
import os
import time

from ndnsf import ServiceUser
from ndnsf_distributed_inference import (
    APPDeployment,
    APPProvider,
    ArtifactProvisioningState,
    NetworkDistributedRepoClient,
    ProviderRuntimeContext,
    SemanticPatternMeta,
    SemanticServiceCacheKey,
    SemanticServiceCacheManager,
    artifact_references_need_repo_client,
    semantic_cache_token_saving_ratio,
)
from ndnsf_distributed_inference.llm_runtime import (
    ManagedLlamaServerRuntime,
    materialize_llm_runtime_artifacts,
)

from pathlib import Path

from llama_server_lib import ROLE, SERVICE, call_llama_server_chat


MODEL_ID = "qwen2.5-0.5b"
TOKENIZER_ID = "qwen-tokenizer"
POLICY_EPOCH = "/Policy/llama-server/v1"
RESPONSE_SCHEMA = "openai-chat-completion-v1"


def _prompt_from_openai_payload(payload: bytes) -> tuple[str, str, int]:
    try:
        doc = json.loads(payload.decode("utf-8"))
    except Exception:
        return payload.decode("utf-8", errors="replace"), MODEL_ID, 64
    messages = doc.get("messages", [])
    prompt_parts = [
        str(item.get("content", ""))
        for item in messages
        if isinstance(item, dict) and item.get("role") == "user"
    ]
    prompt = "\n".join(part for part in prompt_parts if part)
    return (
        prompt or json.dumps(doc, sort_keys=True),
        str(doc.get("model", MODEL_ID) or MODEL_ID),
        int(doc.get("max_tokens", 64) or 64),
    )


def semantic_pattern_for_prompt(prompt: str) -> tuple[str, float]:
    text = prompt.lower()
    if any(word in text for word in ("weather", "rain", "forecast")):
        return "weather-forecast", 0.94
    if "cache" in text or "semantic" in text:
        return "semantic-cache", 0.93
    if "distributed inference" in text or "provider" in text:
        return "distributed-inference", 0.92
    return "general-chat", 0.80


def estimate_prompt_tokens(prompt: str) -> int:
    return max(1, len([part for part in prompt.replace("?", " ").split() if part]))


def default_semantic_patterns() -> list[SemanticPatternMeta]:
    raw = [
        ("semantic-cache", 2, 9, 720),
        ("distributed-inference", 2, 7, 640),
        ("weather-forecast", 1, 8, 384),
        ("general-chat", 1, 20, 64),
    ]
    return [
        SemanticPatternMeta(
            pattern_id=pattern_id,
            conversation_round=round_no,
            query_count=query_count,
            total_prompt_tokens=max(1, query_count * 8),
            total_output_tokens=max(1, query_count * 96),
            estimated_saved_tokens=saved_tokens,
            proportion_ratio=query_count / 44.0,
            token_saving_ratio=semantic_cache_token_saving_ratio(
                saved_tokens=saved_tokens,
                total_tokens=max(1, query_count * 104),
            ),
        )
        for pattern_id, round_no, query_count, saved_tokens in raw
    ]


class LlamaServerSemanticCache:
    def __init__(self, *, enabled: bool = False, budget_mb: float = 32.0):
        self.enabled = bool(enabled)
        self.cache = SemanticServiceCacheManager(
            budget_mb=budget_mb,
            min_admission_score=0.0,
        )
        self.cache.register_patterns(default_semantic_patterns())
        self.requests = 0
        self.hits = 0
        self.misses = 0
        self.saved_tokens = 0
        self.total_tokens = 0

    def key_for_payload(self, payload: bytes) -> tuple[SemanticServiceCacheKey, float, int, int]:
        prompt, model, max_tokens = _prompt_from_openai_payload(payload)
        pattern_id, confidence = semantic_pattern_for_prompt(prompt)
        prompt_tokens = estimate_prompt_tokens(prompt)
        output_tokens = max(1, max_tokens)
        key = SemanticServiceCacheKey(
            service_name=SERVICE,
            model_id=model,
            tokenizer_id=TOKENIZER_ID,
            policy_epoch=POLICY_EPOCH,
            semantic_pattern_id=pattern_id,
            response_schema=RESPONSE_SCHEMA,
            app_namespace="llama-server-provider",
        )
        return key, confidence, prompt_tokens, output_tokens

    def lookup(self, payload: bytes) -> tuple[bytes | None, dict]:
        self.requests += 1
        key, confidence, prompt_tokens, output_tokens = self.key_for_payload(payload)
        self.total_tokens += prompt_tokens + output_tokens
        if not self.enabled:
            self.misses += 1
            return None, {
                "status": "disabled",
                "pattern": key.semantic_pattern_id,
                "confidence": confidence,
                "savedTokens": 0,
            }
        entry = self.cache.get(key, confidence=confidence)
        if entry is None:
            self.misses += 1
            return None, {
                "status": "miss",
                "pattern": key.semantic_pattern_id,
                "confidence": confidence,
                "savedTokens": 0,
            }
        self.hits += 1
        self.saved_tokens += output_tokens
        return entry.response_payload, {
            "status": "hit",
            "pattern": key.semantic_pattern_id,
            "confidence": confidence,
            "savedTokens": output_tokens,
        }

    def admit(self, request_payload: bytes, response_payload: bytes) -> bool:
        if not self.enabled:
            return False
        key, _, prompt_tokens, output_tokens = self.key_for_payload(request_payload)
        entry = self.cache.entry_from_pattern(
            key=key,
            response_payload=response_payload,
            provider="/provider/llama-server",
            confidence_threshold=0.88,
            estimated_prompt_tokens=prompt_tokens,
            estimated_output_tokens=output_tokens,
            byte_count=len(response_payload),
        )
        return self.cache.put(entry)

    def token_saving_ratio(self) -> float:
        return semantic_cache_token_saving_ratio(
            saved_tokens=self.saved_tokens,
            total_tokens=self.total_tokens,
        )


def handle_llama_server(ctx: ProviderRuntimeContext) -> None:
    base_url = os.environ.get("NDNSF_DI_LLAMA_SERVER_URL", "http://127.0.0.1:8080")
    try:
        response = call_llama_server_chat(ctx.request, base_url=base_url)
    except Exception as exc:
        ctx.ndnsf.fail(f"llama-server request failed: {exc}")
        return
    ctx.ndnsf.publish_final_response(response)
    print(
        "LLAMA_SERVER_PROVIDER_RESPONSE",
        f"role={ctx.role}",
        f"bytes={len(response)}",
        f"url={base_url}",
        flush=True,
    )


def make_llama_server_handler(state: ArtifactProvisioningState, base_url: str,
                              semantic_cache: LlamaServerSemanticCache | None = None):
    def handler(ctx: ProviderRuntimeContext) -> None:
        try:
            state.require_ready()
        except Exception as exc:  # noqa: BLE001
            ctx.ndnsf.fail(str(exc))
            return
        cache = semantic_cache or LlamaServerSemanticCache(enabled=False)
        lookup_start = time.perf_counter()
        cached_response, cache_meta = cache.lookup(ctx.request)
        lookup_ms = (time.perf_counter() - lookup_start) * 1000.0
        if cached_response is not None:
            ctx.ndnsf.publish_final_response(cached_response)
            print(
                "LLAMA_SERVER_PROVIDER_SEMANTIC_CACHE",
                "status=hit",
                f"pattern={cache_meta['pattern']}",
                f"savedTokens={cache_meta['savedTokens']}",
                f"lookup_ms={lookup_ms:.3f}",
                f"hit_ratio={cache.hits / max(1, cache.requests):.3f}",
                f"token_saving_ratio={cache.token_saving_ratio():.3f}",
                flush=True,
            )
            return
        try:
            response = call_llama_server_chat(ctx.request, base_url=base_url)
        except Exception as exc:  # noqa: BLE001
            ctx.ndnsf.fail(f"llama-server request failed: {exc}")
            return
        admitted = cache.admit(ctx.request, response)
        ctx.ndnsf.publish_final_response(response)
        print(
            "LLAMA_SERVER_PROVIDER_RESPONSE",
            f"role={ctx.role}",
            f"bytes={len(response)}",
            f"url={base_url}",
            f"semanticCache={cache_meta['status']}",
            f"semanticPattern={cache_meta['pattern']}",
            f"semanticAdmitted={int(bool(admitted))}",
            f"hit_ratio={cache.hits / max(1, cache.requests):.3f}",
            f"token_saving_ratio={cache.token_saving_ratio():.3f}",
            flush=True,
        )
    return handler


ManagedLlamaServer = ManagedLlamaServerRuntime


def materialize_llama_server_artifacts(
    *,
    artifact_references: str,
    cache_dir: str,
    role: str = ROLE,
    repo_client=None,
) -> tuple[Path, Path]:
    model, runtime = materialize_llm_runtime_artifacts(
        artifact_references=artifact_references,
        role=role,
        cache_dir=cache_dir,
        repo_client=repo_client,
    )
    print(
        "LLAMA_SERVER_ARTIFACTS_MATERIALIZED",
        f"role={role}",
        f"model={model}",
        f"runtime={runtime}",
        flush=True,
    )
    return model, runtime


def build_repo_client(deployment, *, provider_id: str, group: str,
                      repo_service: str, upload_prefix: str):
    provider_name = (
        f"{deployment.provider_prefix.rstrip('/')}/{provider_id.strip('/')}"
        if provider_id else deployment.provider_prefix.rstrip("/")
    )
    user = ServiceUser(
        group=group or deployment.group,
        controller=deployment.controller,
        user=provider_name,
        trust_schema=deployment.trust_schema,
        permission_wait_ms=6000,
        handler_threads=1,
        ack_threads=1,
        adaptive_admission=False,
    )
    return NetworkDistributedRepoClient(
        user=user,
        service_name=repo_service,
        upload_prefix=upload_prefix or f"{provider_name}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
        # Artifact materialization normally runs once at provider/session
        # startup. Keep this bootstrap window conservative so a provider that
        # just joined the SVS group does not race repo discovery.
        ack_timeout_ms=8000,
        timeout_ms=60000,
        verbose=True,
    )


def artifact_references_need_repo(path: str, role: str = ROLE) -> bool:
    return artifact_references_need_repo_client(path, role)


def install_llama_server_runtime(
    *,
    deployment,
    provider_id: str,
    group: str,
    artifact_references: str,
    cache_dir: str,
    repo_service: str,
    repo_upload_prefix: str,
    base_url: str,
    no_auto_start: bool,
    extra_args: list[str],
):
    repo_client = None
    if artifact_references_need_repo(artifact_references):
        repo_client = build_repo_client(
            deployment,
            provider_id=provider_id,
            group=group,
            repo_service=repo_service,
            upload_prefix=repo_upload_prefix,
        )
        repo_client.wait_until_ready(30.0, probe_timeout_ms=10000)
    model_path, runtime_path = materialize_llama_server_artifacts(
        artifact_references=artifact_references,
        cache_dir=cache_dir,
        repo_client=repo_client,
    )
    if no_auto_start:
        return None
    managed = ManagedLlamaServer(
        runtime_path,
        model_path,
        base_url,
        extra_args=extra_args,
    )
    managed.start()
    print(
        "LLAMA_SERVER_MANAGED_STARTED",
        f"url={base_url.rstrip('/')}",
        f"model={model_path}",
        flush=True,
    )
    return managed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/tmp/ndnsf-di-llama-server-policy.yaml")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-llama-server-generated")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--group", default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--llama-url", default="http://127.0.0.1:8080")
    parser.add_argument("--artifact-references", default="",
                        help="Repo-backed DI artifact reference JSON containing model and runner")
    parser.add_argument("--artifact-cache-dir", default="/tmp/ndnsf-di-llama-server-cache")
    parser.add_argument("--repo-service", default="/NDNSF/DistributedRepo")
    parser.add_argument("--repo-upload-prefix", default="")
    parser.add_argument("--no-auto-start", action="store_true",
                        help="Materialize artifacts but do not start llama-server")
    parser.add_argument("--sync-materialize-before-serve", action="store_true",
                        help="Wait for artifact install/start before joining the NDNSF service")
    parser.add_argument("--install-timeout-s", type=float, default=300.0)
    parser.add_argument("--materialize-only", action="store_true")
    parser.add_argument("--llama-server-extra-arg", action="append", default=[])
    parser.add_argument("--handler-workers", type=int, default=2)
    parser.add_argument("--enable-semantic-cache", action="store_true",
                        help="Enable provider-local semantic response cache for llama-server")
    parser.add_argument("--semantic-cache-budget-mb", type=float, default=32.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.dry_run:
        print(
            "LLAMA_SERVER_PROVIDER_DRY_RUN",
            f"service={SERVICE}",
            f"roles={args.roles}",
            f"url={args.llama_url}",
            f"artifact_references={args.artifact_references or '(none)'}",
            f"semantic_cache={int(bool(args.enable_semantic_cache))}",
        )
        return 0

    if args.materialize_only:
        if not args.artifact_references:
            raise ValueError("--materialize-only requires --artifact-references")
        deployment = APPDeployment.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
        ).deployment
        repo_client = None
        if artifact_references_need_repo(args.artifact_references):
            repo_client = build_repo_client(
                deployment,
                provider_id=args.provider_id,
                group=args.group,
                repo_service=args.repo_service,
                upload_prefix=args.repo_upload_prefix,
            )
            repo_client.wait_until_ready(30.0, probe_timeout_ms=10000)
        materialize_llama_server_artifacts(
            artifact_references=args.artifact_references,
            cache_dir=args.artifact_cache_dir,
            repo_client=repo_client,
        )
        print("LLAMA_SERVER_MATERIALIZE_ONLY_OK")
        return 0

    provider = APPProvider.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        provider_id=args.provider_id,
        group=args.group,
        handler_workers=args.handler_workers,
    )
    runtime_state = ArtifactProvisioningState(
        component="llama-server runtime",
        initial_status="predeployed",
        initial_message="predeployed llama-server endpoint",
    )
    if args.artifact_references:
        runtime_state.start_install(
            lambda: install_llama_server_runtime(
                deployment=provider.deployment,
                provider_id=args.provider_id,
                group=args.group,
                artifact_references=args.artifact_references,
                cache_dir=args.artifact_cache_dir,
                repo_service=args.repo_service,
                repo_upload_prefix=args.repo_upload_prefix,
                base_url=args.llama_url,
                no_auto_start=args.no_auto_start,
                extra_args=args.llama_server_extra_arg,
            ),
            installing_message="materializing llama-server artifacts",
            ready_message="llama-server ready",
            thread_name="ndnsf-di-llama-install",
            start_marker="LLAMA_SERVER_ASYNC_INSTALL_STARTED",
            fail_marker="LLAMA_SERVER_ASYNC_INSTALL_FAILED",
        )
        if args.sync_materialize_before_serve:
            if not runtime_state.wait_ready(args.install_timeout_s):
                raise RuntimeError("llama-server artifacts did not become ready")
    else:
        runtime_state.mark_ready("external llama-server endpoint")

    try:
        semantic_cache = LlamaServerSemanticCache(
            enabled=args.enable_semantic_cache,
            budget_mb=args.semantic_cache_budget_mb,
        )
        provider.serve_service(
            service=SERVICE,
            roles=args.roles,
            handler=make_llama_server_handler(runtime_state, args.llama_url, semantic_cache),
            backends=["llama.cpp"],
            has_model=True,
            can_provision=False,
            readiness_probe=runtime_state.ack,
        )
        return provider.run()
    finally:
        runtime_state.stop()


if __name__ == "__main__":
    raise SystemExit(main())
