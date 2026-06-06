#!/usr/bin/env python3
"""MiniNDN smoke test for YOLO-style NDNSF-DI layout split inference."""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/testbed(loss=0%).conf"
OUT = REPO / "results/yolo_2x2_minindn_quick"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
CONFIG = OUT / "yolo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-yolo-2x2-policy"
REPO_MANIFEST = OUT / "repo-manifests.json"
APP_ROOT = "/NDNSF-DistributeInference/example"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


def python_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return f"cd {perf.shell_quote(REPO)} && exec python3 {args}"


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    node_env = dict(env)
    node_env["NDNSF_ARTIFACT_CACHE_DIR"] = str(OUT / "artifact-cache" / node.name)
    p = getPopen(node, cmd, envDict=node_env, shell=True, stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def wait_log(path: Path, needle: str, timeout: int = 30, proc=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def validate_repo_manifest_references(path: Path) -> None:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    roles = manifest.get("roles", {})
    if not isinstance(roles, dict) or not roles:
        raise RuntimeError(f"repo manifest has no roles: {path}")
    for role, artifacts in roles.items():
        if not isinstance(artifacts, dict):
            raise RuntimeError(f"repo manifest role {role} is not a mapping")
        for artifact_name in ("model", "runner"):
            entry = artifacts.get(artifact_name)
            if not isinstance(entry, dict):
                raise RuntimeError(f"repo manifest role {role} missing {artifact_name}")
            repo_manifest = entry.get("repoManifest")
            reference = entry.get("largeDataReference")
            if not isinstance(repo_manifest, dict):
                raise RuntimeError(f"repo manifest role {role} {artifact_name} missing repoManifest")
            if not isinstance(reference, dict):
                raise RuntimeError(f"repo manifest role {role} {artifact_name} missing largeDataReference")
            if reference.get("source") != "repo-manifest":
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} unexpected source={reference.get('source')}")
            if reference.get("dataName") != repo_manifest.get("objectName"):
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} dataName/objectName mismatch")
            expected_digest = "sha256:" + str(repo_manifest.get("sha256", ""))
            if reference.get("digest") != expected_digest:
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} digest mismatch")
            if int(reference.get("plaintextSize", -1)) != int(repo_manifest.get("size", -2)):
                raise RuntimeError(
                    f"repo manifest role {role} {artifact_name} size mismatch")
    print(f"YOLO_2X2_REPO_MANIFEST_REFERENCES_OK path={path}")


def read_node_traffic(node) -> dict[str, int]:
    script = r"""
rx=0
tx=0
for d in /sys/class/net/*; do
  iface=$(basename "$d")
  [ "$iface" = "lo" ] && continue
  r=$(cat "$d/statistics/rx_bytes" 2>/dev/null || echo 0)
  t=$(cat "$d/statistics/tx_bytes" 2>/dev/null || echo 0)
  rx=$((rx + r))
  tx=$((tx + t))
done
echo "$rx $tx"
"""
    output = node.cmd("sh -c {}".format(perf.shell_quote(script))).strip()
    numbers = re.findall(r"\d+", output)
    if len(numbers) < 2:
        return {"rxBytes": 0, "txBytes": 0}
    return {"rxBytes": int(numbers[-2]), "txBytes": int(numbers[-1])}


def snapshot_traffic(ndn) -> dict[str, dict[str, int]]:
    return {
        node.name: read_node_traffic(node)
        for node in ndn.net.hosts
    }


def write_traffic_delta(layout: str, phase: str,
                        before: dict[str, dict[str, int]],
                        after: dict[str, dict[str, int]]) -> dict:
    nodes = {}
    total_rx = 0
    total_tx = 0
    for name in sorted(set(before) | set(after)):
        start = before.get(name, {"rxBytes": 0, "txBytes": 0})
        end = after.get(name, {"rxBytes": 0, "txBytes": 0})
        rx = max(0, int(end.get("rxBytes", 0)) - int(start.get("rxBytes", 0)))
        tx = max(0, int(end.get("txBytes", 0)) - int(start.get("txBytes", 0)))
        nodes[name] = {"rxBytes": rx, "txBytes": tx, "totalBytes": rx + tx}
        total_rx += rx
        total_tx += tx
    summary = {
        "layout": layout,
        "phase": phase,
        "rxBytes": total_rx,
        "txBytes": total_tx,
        "totalNodeBytes": total_rx + total_tx,
        "nodes": nodes,
    }
    path = OUT / "traffic-stats.json"
    history = []
    if path.exists():
        try:
            history = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            history = []
    if not isinstance(history, list):
        history = []
    history.append(summary)
    path.write_text(json.dumps(history, indent=2, sort_keys=True), encoding="utf-8")
    print(
        "YOLO_LAYOUT_TRAFFIC "
        f"layout={layout} phase={phase} "
        f"rx_bytes={total_rx} tx_bytes={total_tx} "
        f"total_node_bytes={total_rx + total_tx} path={path}"
    )
    return summary


def load_policy_roles(path: Path) -> list[str]:
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("PyYAML is required to read generated DI policies") from exc
    config = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    for service in config.get("services", []):
        name = str(service.get("name", ""))
        if name != "/NDNSF/DistributedRepo":
            roles = [str(role) for role in service.get("roles", []) if str(role)]
            if roles:
                return roles
    raise RuntimeError(f"no inference service roles found in {path}")


def provider_role_assignments(roles: list[str]) -> list[tuple[str, str, list[str]]]:
    nodes = ["ucla", "wustl", "uiuc", "umich", "arizona", "caida", "pku", "neu", "csu"]
    provider_ids = ["", "A", "B", "C", "E", "F", "G", "H", "I"]
    if len(roles) > len(nodes):
        raise RuntimeError(
            f"layout needs {len(roles)} role providers but MiniNDN smoke "
            f"defines only {len(nodes)} provider nodes")
    result = []
    for index, role in enumerate(roles):
        node_name = nodes[index]
        provider_id = provider_ids[index]
        name = "provider-root" if not provider_id else f"provider-{provider_id}"
        result.append((node_name, name, [
            "--provider-id", provider_id,
            "--roles", role,
        ]))
    return result


def stop(procs):
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def provider_identity(provider_id: str) -> str:
    return PROVIDER_PREFIX if not provider_id else PROVIDER_PREFIX + "/" + provider_id


def initialize_di_keychains(ndn, output_dir: Path, provider_identities: list[str]) -> None:
    """Install root-signed keys that match the generated DI policy namespace."""
    log("Installing root-signed DI keychain material on MiniNDN nodes")
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)
    identities = [
        CONTROLLER_IDENTITY,
        PROVIDER_PREFIX + "/D",
        USER_IDENTITY,
        *provider_identities,
    ]
    identities = list(dict.fromkeys(identities))

    for node in ndn.net.hosts:
        for identity in [APP_ROOT] + identities:
            perf.node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(identity)))

    controller = ndn.net["csu"]
    root_cert_path = security_dir / "root.cert"
    perf.node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
        perf.shell_quote(APP_ROOT), perf.shell_quote(root_cert_path)))
    perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
        perf.shell_quote(root_cert_path)))
    log("di_root_cert identity={} name={} file={}".format(
        APP_ROOT, perf.certificate_name_from_file(root_cert_path), root_cert_path))

    exported_keys = []
    for index, identity in enumerate(identities):
        cert_path = security_dir / f"di-identity-{index}.cert"
        req_path = security_dir / f"di-identity-{index}.req"
        key_path = security_dir / f"di-identity-{index}.ndnkey"
        perf.node_cmd(controller, "ndnsec key-gen -n -t r {} > {}".format(
            perf.shell_quote(identity), perf.shell_quote(req_path)))
        perf.node_cmd(controller, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
            perf.shell_quote(APP_ROOT), perf.shell_quote(req_path), perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec-export -P 123456 -o {} -i {}".format(
            perf.shell_quote(key_path), perf.shell_quote(identity)))
        exported_keys.append(key_path)

    for node in ndn.net.hosts:
        perf.node_cmd(node, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(root_cert_path)))
        for key_path in exported_keys:
            perf.node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(key_path)))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--layout", default="2x2",
                        help="YOLO stage-by-shard layout, e.g. 1x3, 2x3, 3x2, 3x3")
    args_cli = parser.parse_args()
    layout = args_cli.layout.strip().lower().replace("*", "x")
    sys.argv = [sys.argv[0]]
    safe_layout = layout.replace("/", "-")
    global OUT, CONFIG, GEN_POLICY, REPO_MANIFEST
    OUT = REPO / f"results/yolo_{safe_layout}_minindn_quick"
    CONFIG = OUT / "yolo_policy.yaml"
    GEN_POLICY = f"/tmp/ndnsf-di-yolo-{safe_layout}-policy"
    REPO_MANIFEST = OUT / "repo-manifests.json"

    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    py_path = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(PY_DIR),
        os.environ.get("PYTHONPATH", ""),
    ])
    subprocess.run([
        "python3",
        str(PY_DIR / "split_model.py"),
        "--auto-split",
        "--layout",
        layout,
        "--out-dir",
        str(OUT / "model"),
        "--policy",
        str(CONFIG),
        "--dynamic-provisioning",
        "--trust-anchor-file",
        str(OUT / "security/root.cert"),
    ], cwd=str(REPO), env={**os.environ, "PYTHONPATH": py_path}, check=True)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="csu",
        user_node="memphis",
        providers=5,
        provider_nodes="ucla,wustl,uiuc,umich,neu",
        serve_provider_certs=False,
        debug_ack=False,
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=True,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=1,
        performance_mode=False,
    )
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        providers = provider_role_assignments(load_policy_roles(CONFIG))
        provider_identities = [
            provider_identity(argv[argv.index("--provider-id") + 1])
            for _, _, argv in providers
        ]

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net["csu"]], [
            "/NDNSF-DistributeInference/example/controller",
            "/NDNSF-DistributeInference/example/controller/DKEY",
            "/NDNSF-DistributeInference/example/controller/KEY",
            "/NDNSF-DistributeInference/example/group",
        ])
        rh.addOrigin([ndn.net["memphis"]], ["/NDNSF-DistributeInference/example/user", "/NDNSF-DistributeInference/example/group"])
        origins = [
            (
                node_name,
                provider_identity(argv[argv.index("--provider-id") + 1]),
            )
            for node_name, _, argv in providers
        ]
        origins.append(("neu", "/NDNSF-DistributeInference/example/provider/D"))
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/NDNSF-DistributeInference/example/group"])
        rh.addOrigin([ndn.net["neu"]], ["/NDNSF/DistributedRepo/Object"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        initialize_di_keychains(ndn, OUT, provider_identities)
        subprocess.run(["rm", "-rf", str(OUT / "artifact-cache")], check=False)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)
        env["NDNSF_HANDLER_THREADS"] = "1"
        env["NDNSF_ACK_THREADS"] = "1"
        env["NDNSF_SVS_ASYNC_PUBLISH"] = "0"
        env["NDNSF_SVS_PARALLEL_SYNC"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = "0"
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(PY_DIR),
            "/home/tianxing/.local/lib/python3.8/site-packages",
            "/usr/local/lib/python3.8/dist-packages",
            "/usr/lib/python3/dist-packages",
            os.environ.get("PYTHONPATH", ""),
        ])

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        _, controller_log = start(ndn.net["csu"], "controller",
                                  python_cmd("controller.py", common), env, procs)
        if not wait_log(controller_log, "ServiceController listening", 20):
            raise RuntimeError(f"controller did not become ready; see {controller_log}")
        time.sleep(4)
        _, repo_log = start(
            ndn.net["neu"],
            "repo",
            python_cmd("repo_node.py", common + [
                "--provider-id", "D",
                "--repo-node", "/NDNSF-DistributeInference/example/provider/D",
                "--failure-domain", "repo-rack",
                "--storage-dir", f"/tmp/yolo-{safe_layout}-repo-store",
                "--handler-threads", "1",
                "--ack-threads", "1",
            ]),
            env,
            procs,
        )
        if not wait_log(repo_log, "Installed provider permission", 60):
            raise RuntimeError(f"repo did not install permissions; see {repo_log}")
        deployer_proc, deployer_log = start(ndn.net["csu"], "controller-deployer",
                                            python_cmd("controller.py", common + [
                                                "--deploy-only",
                                                "--deploy-to-repo-manifest",
                                                str(REPO_MANIFEST),
                                                "--replication-factor",
                                                "1",
                                            ]), env, procs)
        if not wait_log(deployer_log, "YOLO_2X2_CONTROLLER_REPO_DEPLOYED", 360,
                        proc=deployer_proc):
            deployer_rc = deployer_proc.poll()
            deployer_tail = deployer_log.read_text(errors="replace")[-2000:] if deployer_log.exists() else ""
            raise RuntimeError(
                "controller did not deploy repo artifacts; "
                f"returncode={deployer_rc}; see {deployer_log}\n{deployer_tail}")
        validate_repo_manifest_references(REPO_MANIFEST)

        for node_name, name, argv in providers:
            _, lp = start(ndn.net[node_name], name,
                          python_cmd("provider.py", common + argv + [
                              "--dynamic-provisioning",
                              "--temp-dir",
                              f"/tmp/{name}",
                          ]),
                          env, procs)
            if not wait_log(lp, "Installed provider permission", 20):
                raise RuntimeError(f"{name} did not install permissions; see {lp}")
            time.sleep(0.5)

        time.sleep(2)
        user_common = common + [
            "--repo-manifest-file",
            str(REPO_MANIFEST),
            "--ack-timeout-ms", "1500",
            "--timeout-ms", "60000",
            "--sequential-requests", "1",
        ]
        cold_traffic_start = snapshot_traffic(ndn)
        user_proc, user_log = start(
            ndn.net["memphis"],
            "user-cold",
            python_cmd("user.py", user_common),
            env,
            procs,
        )
        user_proc.wait(timeout=90)
        cold_traffic_end = snapshot_traffic(ndn)
        write_traffic_delta(layout, "cold", cold_traffic_start, cold_traffic_end)
        cold_text = user_log.read_text(errors="replace")
        print(cold_text)
        if "YOLO_LAYOUT_RESULT" not in cold_text or "ok=true" not in cold_text:
            raise RuntimeError(
                f"YOLO {layout} cold provisioning failed rc={user_proc.returncode}; log={user_log}")

        warm_traffic_start = snapshot_traffic(ndn)
        warm_proc, warm_log = start(
            ndn.net["memphis"],
            "user-warm",
            python_cmd("user.py", common + [
                "--repo-manifest-file",
                str(REPO_MANIFEST),
                "--ack-timeout-ms", "1500",
                "--timeout-ms", "60000",
                "--sequential-requests", "1",
            ]),
            env,
            procs,
        )
        warm_proc.wait(timeout=90)
        warm_traffic_end = snapshot_traffic(ndn)
        write_traffic_delta(layout, "warm", warm_traffic_start, warm_traffic_end)
        warm_text = warm_log.read_text(errors="replace")
        print(warm_text)
        provider_text = "\n".join(
            (OUT / f"{name}.log").read_text(errors="replace")
            for _, name, _ in providers
        )
        success = (
            "YOLO_LAYOUT_RESULT" in warm_text and
            "ok=true" in warm_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS" in provider_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_HIT" in provider_text
        )
        if not success:
            raise RuntimeError(
                f"YOLO {layout} dynamic provisioning/cache validation failed; "
                f"cold={user_log} warm={warm_log}")
        print(
            f"YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK "
            f"layout={layout} cold={user_log} warm={warm_log}"
        )
        if layout == "2x2":
            print(f"YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK cold={user_log} warm={warm_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
