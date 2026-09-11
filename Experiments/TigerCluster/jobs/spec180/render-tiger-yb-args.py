#!/usr/bin/env python3
"""Render the candidate-bound Tiger Y-B process argument files.

This renderer is deliberately a small, side-effect-free boundary: it reads
the signed workload document (the sealed candidate inputs) and emits the
per-process argument files plus the node-local NFD configuration that the
in-image launcher consumes.  It never invokes NFD, starts a process, or
mutates the bundle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys


PROVIDER_ROLES = {
    "BackboneNeck": ("BackboneNeck",),
    "DetectShard0": ("DetectShard0",),
    "DetectShard1": ("DetectShard1",),
    "Merge": ("Merge",),
}
APP_DIR = "/opt/ndnsf-di/replay/repo/examples/python/NDNSF-DistributedInference/yolo_2x2"


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def fail(message: str) -> None:
    raise SystemExit("SPEC180_TIGER_RENDER_" + message)


def load_workload(path: Path) -> dict:
    try:
        raw = path.read_bytes()
    except OSError as exc:
        fail("WORKLOAD_READ_FAILED:" + str(exc))
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        fail("WORKLOAD_NOT_JSON:" + str(exc))
    if not isinstance(value, dict) or value.get("schema") != (
            "spec180-dispatch-workload-v1"):
        fail("WORKLOAD_SCHEMA_UNSUPPORTED")
    if value.get("gate") != "yolo-functional" or value.get("case") != "Y-B":
        fail("WORKLOAD_GATE_MISMATCH")
    environment = value.get("environment")
    if not isinstance(environment, dict):
        fail("WORKLOAD_ENVIRONMENT_MISSING")
    for name in ("SPEC180_YOLO_CANONICAL_PACKAGE",
                 "SPEC180_YOLO_CATALOGUE_REGISTRY",
                 "SPEC180_YOLO_OFFER_TRUST_ROOT",
                 "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP",
                 "SPEC180_YOLO_CATALOG_DATA_NAME",
                 "SPEC180_YOLO_CATALOG_SIGNER",
                 "SPEC180_YOLO_NATIVE_REQUESTER_CONFIG",
                 "SPEC180_YOLO_TOPOLOGY",
                 "SPEC180_YOLO_CONFIG"):
        if not environment.get(name):
            fail("WORKLOAD_ENVIRONMENT_INCOMPLETE:" + name)
    return environment


def write_args(path: Path, argv: list[str]) -> str:
    path.write_text("".join(arg + "\n" for arg in argv), encoding="utf-8")
    return digest_bytes(path.read_bytes())


def _render_runtime_publication(
    path: Path, environment: dict, output_dir: Path,
) -> None:
    """Build the candidate-bound controller publication batch."""
    # Import the compiled runtime packages from the SIF site-packages first:
    # the runner module inserts its own repo-relative paths at import time,
    # and those source copies have no compiled bindings.
    import ndnsf  # noqa: F401
    import ndnsf_distributed_inference  # noqa: F401
    import py_repoclient  # noqa: F401
    repo = Path("/opt/ndnsf-di/replay/repo")
    sys.path.insert(0, str(repo / "Experiments"))
    sys.path.insert(0, str(repo / "examples/python"))
    import NDNSF_DI_YoloAckDriven_Minindn as runner

    package = Path(environment["SPEC180_YOLO_CANONICAL_PACKAGE"])
    registry = environment["SPEC180_YOLO_CATALOGUE_REGISTRY"]

    class FakeBinding:
        case = "Y-B"
        identities = {
            "controller": environment["SPEC180_YOLO_CATALOG_SIGNER"],
        }
        output = Path("/evidence")

    import json as _json
    manifest = _json.loads(
        (package / "manifest.json").read_text(encoding="utf-8"))
    inputs = {
        "package": package,
        "registry": registry,
        "manifest": manifest,
        "descriptor": {
            "catalogueDataName":
                environment["SPEC180_YOLO_CATALOG_DATA_NAME"],
            "catalogueSigner":
                environment["SPEC180_YOLO_CATALOG_SIGNER"],
        },
    }
    built = runner.build_runtime_publication_file(FakeBinding(), inputs)
    target = output_dir / "runtime-publication.json"
    target.write_text(
        built.read_text(encoding="utf-8"), encoding="utf-8")
    if str(path) != str(target):
        path.write_text(target.read_text(encoding="utf-8"), encoding="utf-8")


def render(workload: Path, output_dir: Path, model_root: str) -> dict:
    environment = load_workload(workload)
    # Match the launcher's fixed identity inventory before writing any argv.
    # Placement is still request/ACK-owned; this is only the startup profile.
    try:
        policy = json.loads(Path(environment["SPEC180_YOLO_CONFIG"]).read_text())
        topology_path = Path(environment["SPEC180_YOLO_TOPOLOGY"])
        if not topology_path.is_file():
            fail("TOPOLOGY_INPUT_MISSING")
        native_requester_config = Path(
            environment["SPEC180_YOLO_NATIVE_REQUESTER_CONFIG"])
        if not native_requester_config.is_file():
            fail("NATIVE_REQUESTER_CONFIG_MISSING")
        services = [service for service in policy["services"]
                    if not service["name"].startswith("/NDNSF/DistributedRepo")]
        if len(services) != 1:
            fail("INFERENCE_SERVICE_AMBIGUOUS")
        service = services[0]
        expected = {"/example/provider/" + role: [role] for role in PROVIDER_ROLES}
        providers = service["providers"]
        if (len(providers) != len(expected) or
                {p["identity"]: p["roles"] for p in providers} != expected or
                policy["controller"] != "/example/controller" or
                policy["controller"] != environment["SPEC180_YOLO_CATALOG_SIGNER"] or
                not isinstance(policy["group"], str) or
                not policy["group"].startswith("/") or
                not service["name"].startswith("/")):
            fail("PROCESS_PROFILE_MISMATCH")
        runtime = policy.get("runtime", {})
        if runtime and not isinstance(runtime, dict):
            fail("RUNTIME_NODE_MAP_INVALID")
        runtime_nodes = runtime.get("nodes", {}) if runtime else {}
        if runtime_nodes and not isinstance(runtime_nodes, dict):
            fail("RUNTIME_NODE_MAP_INVALID")
        declared_nodes = set()
        for key, value in runtime_nodes.items():
            values = value.values() if key == "providers" else (value,)
            if key == "providers" and not isinstance(value, dict):
                fail("RUNTIME_NODE_MAP_INVALID")
            for node in values:
                if not isinstance(node, str) or not node:
                    fail("RUNTIME_NODE_MAP_INVALID")
                declared_nodes.add(node)
        # This entrypoint starts one NFD and all four Providers in one Slurm
        # task.  A MiniNDN case may carry a richer node map, but silently
        # collapsing it here would make the Tiger result describe a topology
        # that was never executed.
        if len(declared_nodes) > 1:
            fail("MULTI_NODE_RUNTIME_UNSUPPORTED")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        fail("CASE_CONFIG_INVALID:" + type(exc).__name__)
    output = output_dir
    output.mkdir(parents=True, exist_ok=True)
    package = environment["SPEC180_YOLO_CANONICAL_PACKAGE"]
    registry = environment["SPEC180_YOLO_CATALOGUE_REGISTRY"]
    case_config = environment["SPEC180_YOLO_CONFIG"]
    output_root = Path("/evidence")
    # The Tiger staging lays the operator-owned secret material at fixed
    # model-root paths; the workload environment binds only the public
    # candidate identity fields.
    private_key_map = {
        "/example/provider/" + suffix:
            f"{model_root}/offer-private-keys/{suffix}.pem"
        for suffix in PROVIDER_ROLES
    }
    envelope_key_file = f"{model_root}/request-envelope.key"
    common = [
        "--config", case_config,
        "--generated-policy-dir", str(output_root / "generated-policy"),
    ]
    renders = {}

    controller_args = [
        "/opt/venv/bin/python", f"{APP_DIR}/controller.py",
        *common,
        "--spec180-runtime-publication-file",
        str(Path("/evidence") / "runtime-publication.json"),
    ]
    renders["controller"] = write_args(
        output / "controller.args", controller_args)

    repo_args = [
        "/opt/venv/bin/python", f"{APP_DIR}/repo_node.py", *common,
        "--provider-id", "Repo", "--repo-node", "/example/provider/Repo",
        "--failure-domain", "spec180-repo", "--storage-dir", "/evidence/repo-store",
        "--handler-threads", "1", "--ack-threads", "1",
    ]
    renders["repo"] = write_args(output / "repo.args", repo_args)

    provider_dir = output / "providers"
    provider_dir.mkdir(exist_ok=True)
    for identity_suffix, roles in PROVIDER_ROLES.items():
        is_merge = identity_suffix == "Merge"
        generated = output_root / "generated-policy"
        provider_args = [
            "/usr/bin/env", "CUDA_VISIBLE_DEVICES=" + ("" if is_merge else "0"),
            "/opt/ndnsf-di/current/bin/di-native-provider", "--serve",
            "--plan", str(generated / "native-execution-plan.json"),
            "--manifest", str(generated / "service-manifest.json"),
            "--service", service["name"],
            "--provider", "/example/provider/" + identity_suffix,
            "--group", policy["group"],
            "--controller", policy["controller"],
            "--trust-schema", str(generated / "trust-schema.conf"),
            "--roles", ",".join(roles),
            "--workers", "1", "--handler-threads", "1", "--ack-threads", "1",
            "--artifact-cache-dir", str(output_root / "native-artifact-cache" / identity_suffix),
            "--selection-offer-key-file",
            private_key_map.get(
                "/example/provider/" + identity_suffix, ""),
            "--offer-backend", "onnxruntime-cpu" if is_merge else "onnxruntime-cuda",
            "--offer-device", "cpu" if is_merge else "cuda:0",
            "--offer-can-provision", "--permission-wait-ms", "60000",
        ]
        renders["provider-" + identity_suffix] = write_args(
            provider_dir / (identity_suffix + ".args"), provider_args)

    user_args = [
        "/opt/venv/bin/python", f"{APP_DIR}/user.py",
        *common,
        "--canonical-package", package,
        "--catalogue-registry", registry,
        "--offer-trust-root", environment["SPEC180_YOLO_OFFER_TRUST_ROOT"],
        "--offer-public-key-map", environment["SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP"],
        "--catalog-data-name", environment["SPEC180_YOLO_CATALOG_DATA_NAME"],
        "--catalog-signer", environment["SPEC180_YOLO_CATALOG_SIGNER"],
        "--native-requester-config",
        str(native_requester_config),
        "--ack-timeout-ms", "1500",
        "--timeout-ms", "60000",
        "--input-size", "640",
        "--native-tensor-input",
        "--envelope-key-file", envelope_key_file,
    ]
    renders["user"] = write_args(output / "user.args", user_args)

    # The controller's runtime publication batch is normally built by the
    # MiniNDN runner on the host.  On Tiger there is no runner process, so
    # the renderer produces the identical candidate-bound batch with the
    # runner's own publication builder.
    publication_path = output / "runtime-publication.json"
    _render_runtime_publication(
        publication_path, environment, output_dir)
    renders["runtimePublication"] = digest_bytes(
        publication_path.read_bytes())

    nfd_path = output / "nfd.conf"
    nfd_path.write_text(
        "; SPEC180 Tiger node-local NFD configuration\n"
        "general\n{\n}\n"
        "log\n{\n  default_level INFO\n}\n"
        "face_system\n{\n"
        "  unix\n  {\n    path /evidence/runtime/run/nfd.sock\n  }\n"
        "}\n"
        "tables\n{\n"
        "  cs_max_packets 65536\n"
        "  cs_policy lru\n"
        "  cs_unsolicited_policy drop-all\n"
        "}\n"
        "rib\n{\n"
        "  auto_prefix_propagate\n  {\n"
        "    cost 15\n    timeout 10000\n"
        "    refresh_interval 300\n"
        "    base_retry_wait 50\n"
        "    max_retry_wait 3600\n"
        "  }\n"
        "}\n"
        "authorizations\n{\n"
        "  authorize\n  {\n"
        "    certfile any\n"
        "    privileges\n    {\n"
        "      faces\n"
        "      fib\n"
        "      cs\n"
        "      strategy-choice\n"
        "    }\n"
        "  }\n"
        "}\n"
        "forwarder\n{\n  default_hop_limit 64\n}\n",
        encoding="utf-8",
    )
    renders["nfdConf"] = digest_bytes(nfd_path.read_bytes())

    result = {
        "schema": "spec180-yolo-workload-render-v1",
        "status": "PASS",
        "renders": renders,
        "modelRoot": str(model_root),
        "deploymentScope": "single-node-native-requester",
        "topologyDigest": digest_bytes(
            Path(environment["SPEC180_YOLO_TOPOLOGY"]).read_bytes()),
    }
    (output / "render-manifest.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--model-root", default="/models")
    args = parser.parse_args(argv)
    result = render(args.workload, args.output_dir, args.model_root)
    print("SPEC180_TIGER_ARGS_RENDERED " + json.dumps({
        "status": result["status"],
        "renders": result["renders"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
