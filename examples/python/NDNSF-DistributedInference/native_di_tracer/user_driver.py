#!/usr/bin/env python3
"""Submit a real NDNSF collaboration request for /Inference/NativeTracer."""

from __future__ import annotations

import argparse
import json
import secrets
import struct
import sys
import time
from pathlib import Path

from ndnsf import ServiceUser


SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"


def encode_tensor_bundle() -> bytes:
    payload = bytearray(b"NDITB001")
    payload += struct.pack("<I", 1)
    name = b"images"
    payload += struct.pack("<I", len(name)) + name
    payload += struct.pack("<I", 1)  # Float32
    shape = [1, 3, 2, 2]
    payload += struct.pack("<I", len(shape))
    for dim in shape:
        payload += struct.pack("<q", dim)
    values = [float(i) / 10.0 for i in range(12)]
    data = struct.pack("<" + "f" * len(values), *values)
    payload += struct.pack("<Q", len(data)) + data
    return bytes(payload)


def load_service_plan(path: Path, service: str) -> dict:
    plan = json.loads(path.read_text(encoding="utf-8"))
    return next(item for item in plan["services"] if item["service"] == service)


def sample_service_plan(service: str) -> dict:
    return {
        "service": service,
        "roles": ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"],
        "dependencies": [
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/0"],
                "keyScope": "backbone-to-head0",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Backbone"],
                "consumers": ["/Head/Shard/1"],
                "keyScope": "backbone-to-head1",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/0"],
                "consumers": ["/Merge"],
                "keyScope": "head0-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
            {
                "producers": ["/Head/Shard/1"],
                "consumers": ["/Merge"],
                "keyScope": "head1-to-merge",
                "topicPrefix": "/activation",
                "required": True,
            },
        ],
    }


def collaboration_roles(service_plan: dict, service: str) -> list[dict]:
    return [
        {
            "role": role,
            "service": service,
            "min_providers": 1,
            "max_providers": 1,
        }
        for role in service_plan["roles"]
    ]


def collaboration_dependencies(service_plan: dict) -> list[dict]:
    deps = []
    for dep in service_plan.get("dependencies", []):
        deps.append({
            "producers": list(dep.get("producers", [])),
            "consumers": list(dep.get("consumers", [])),
            "key_scope": str(dep["keyScope"]),
            "topic_prefix": str(dep.get("topicPrefix", "/activation")),
            "required": bool(dep.get("required", True)),
        })
    return deps


def key_scopes_and_role_scopes(service_plan: dict) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    key_scopes: dict[str, list[str]] = {}
    role_scopes: dict[str, list[str]] = {role: [] for role in service_plan["roles"]}
    for dep in service_plan.get("dependencies", []):
        scope = str(dep["keyScope"])
        roles = list(dep.get("producers", [])) + list(dep.get("consumers", []))
        key_scopes[scope] = roles
        for role in roles:
            role_scopes.setdefault(role, []).append(scope)
    return key_scopes, role_scopes


def publish_scope_keys(user: ServiceUser, service: str, key_scopes: dict[str, list[str]]) -> dict[str, str]:
    scope_key_data_names: dict[str, str] = {}
    for scope in key_scopes:
        result = user.publish_encrypted_large_data(
            service,
            secrets.token_bytes(32),
            object_label=f"native-tracer-scope-key-{scope}",
            freshness_ms=60000,
        )
        if not result.success:
            raise RuntimeError(f"scope key publish failed for {scope}: {result.error}")
        scope_key_data_names[scope] = result.encrypted_data_name
    return scope_key_data_names


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the NativeTracer user driver")
    parser.add_argument("--plan", default="")
    parser.add_argument("--service", default=SERVICE)
    parser.add_argument("--group", default=GROUP)
    parser.add_argument("--controller", default=CONTROLLER)
    parser.add_argument("--user", default=USER)
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--ack-timeout-ms", type=int, default=1200)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.plan:
        service_plan = load_service_plan(Path(args.plan), args.service)
    elif args.dry_run:
        service_plan = sample_service_plan(args.service)
    else:
        raise SystemExit("--plan is required unless --dry-run is used")
    roles = collaboration_roles(service_plan, args.service)
    dependencies = collaboration_dependencies(service_plan)
    key_scopes, role_scopes = key_scopes_and_role_scopes(service_plan)
    if args.dry_run:
        print(json.dumps({
            "service": args.service,
            "roles": roles,
            "dependencies": dependencies,
            "keyScopes": key_scopes,
            "roleScopes": role_scopes,
        }, indent=2, sort_keys=True))
        return 0

    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=args.trust_schema,
        permission_wait_ms=args.permission_wait_ms,
        serve_certificates=True,
    )
    allowed = [entry.service for entry in user.get_allowed_services()]
    print("NDNSF_DI_NATIVE_TRACER_USER_ALLOWED " + json.dumps(allowed), flush=True)
    if args.service not in allowed:
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": f"missing user permission for {args.service}; allowed={allowed}",
            "elapsedMs": 0.0,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 1
    scope_key_data_names = publish_scope_keys(user, args.service, key_scopes)
    print(
        "NDNSF_DI_NATIVE_TRACER_SCOPE_KEYS "
        + json.dumps(scope_key_data_names, sort_keys=True),
        flush=True,
    )
    start = time.perf_counter()
    try:
        response = user.request_collaboration(
            args.service,
            encode_tensor_bundle(),
            roles=roles,
            key_scopes=key_scopes,
            dependencies=dependencies,
            scope_key_data_names=scope_key_data_names,
            role_scopes=role_scopes,
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        result = {
            "status": "executed" if response.status else "failed",
            "service": args.service,
            "responseStatus": bool(response.status),
            "payloadBytes": len(response.payload),
            "error": response.error,
            "elapsedMs": elapsed_ms,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 0 if response.status else 1
    except Exception as exc:
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        result = {
            "status": "failed",
            "service": args.service,
            "responseStatus": False,
            "payloadBytes": 0,
            "error": str(exc),
            "elapsedMs": elapsed_ms,
        }
        print("NDNSF_DI_NATIVE_TRACER_USER_EXECUTION " + json.dumps(result, sort_keys=True), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
