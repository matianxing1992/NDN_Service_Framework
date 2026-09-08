"""Spec183 T010: bounded CPU MiniNDN Y-B run with the real provision outputs.

Maps the containerized-issuer products (case.json, offer trust/maps, role
certificates, catalogue names) plus the fixed experiment offer private keys
onto the maintained ACK-driven MiniNDN driver
(``Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B``), which runs
the four-role graph over the clean-root host binaries.  This produces the
real host MiniNDN receipt; it is not a GPU or SIF qualification.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Iterable

_TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TOOL_DIR.parent))  # TigerCluster
_REPO_ROOT = Path(__file__).resolve().parents[3]
TC = _REPO_ROOT / "Experiments/TigerCluster"
PROFILE_REL = TC / "profiles/yolo-two-node.json"
TOPOLOGY_REL = (_REPO_ROOT / "specs/181-ndnsf-di-protected-grant-qualification"
                / "contracts/local-case-configs/topology.conf")
DRIVER_REL = _REPO_ROOT / "Experiments/NDNSF_DI_YoloAckDriven_Minindn.py"
ROLES = ("BackboneNeck", "DetectShard0", "DetectShard1", "Merge")


def _prepared(output: Path, run_id: str) -> dict:
    receipt = Path(output) / run_id / "prepare.json"
    value = json.loads(receipt.read_text())
    if value.get("status") != "PREPARED":
        raise SystemExit(f"prepared run receipt invalid at {receipt}")
    return value


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=("Y-B",), default="Y-B")
    parser.add_argument("--library-path", default="/tmp/t008-build-root/lib")
    args = parser.parse_args(argv)

    output = Path(args.output).resolve()
    run_root = output / args.run_id
    public = run_root / "public"
    private = run_root / "private"
    prepared = _prepared(output, args.run_id)
    from runtime.yolo_profile import _read_plane
    receipt = _read_plane(public / "preparation.json")
    case_path = public / "case.json"
    case = _read_plane(case_path)
    identities = case["runtime"]["identities"]

    keys = TC / ".keys/offers"
    private_map = {identities[role]: str(keys / (role + ".key"))
                   for role in ROLES}
    for path in private_map.values():
        if not Path(path).is_file():
            raise SystemExit(f"offer private key missing: {path}")
    # The provision-stage key map records in-container paths (/config/...);
    # the digest keys are authoritative, so re-map values to the host copy.
    offer_map = _read_plane(public / "offer-public-key-map.json")
    host_offer_map = {
        digest: str(public / "offers" / Path(value).name)
        for digest, value in offer_map.items()}
    # The MiniNDN driver needs a deployment config in the spec181 Y-B layout:
    # the Spec183 identities from case.json plus a MiniNDN node placement.
    namespace = prepared["plan"]["namespace"]
    provider_identities = {identities[role]: identities[role] for role in ROLES}
    deploy = json.loads(json.dumps(case))
    deploy["runtime"]["nodes"] = {
        "controller": "memphis", "user": "memphis", "repo": "neu",
        "providers": {
            identities["BackboneNeck"]: "ucla",
            identities["DetectShard0"]: "neu",
            identities["DetectShard1"]: "arizona",
            identities["Merge"]: "wustl",
        },
    }
    deploy["runtime"]["identities"] = {
        "controller": identities["controller"],
        "group": case["group"],
        "providerPrefix": namespace,
        "providers": provider_identities,
        "repo": identities["repo"],
        "repoServicePrefix": "/NDNSF/DistributedRepo/Object",
        "user": identities["user"],
    }
    deploy["runtime"]["provider_prefix"] = namespace
    deploy["runtime"]["user_identity"] = identities["user"]

    state = TC / ".cache/t010-minindn-state"
    case_output = TC / ".cache/t010-minindn-output"
    inputs = TC / ".cache/t010-minindn-inputs"
    for directory in (state, inputs):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    # The driver demands an exclusive, initially empty output root.
    case_output.mkdir(mode=0o700, parents=True, exist_ok=False)

    env = dict(os.environ)
    env.update({
        "LD_LIBRARY_PATH": args.library_path,
        "NDNSF_DI_STATE_ROOT": str(state),
        "NDNSF_DI_ENVELOPE_KEY_FILE": str(private / "user/request-envelope.key"),
        "SPEC180_CASE_OUTPUT_DIR": str(case_output),
        "SPEC180_YOLO_CANONICAL_PACKAGE": str(_REPO_ROOT / "Experiments/TigerCluster"
                                             / ".cache/model/spec183-signed/canonical-package"),
        "SPEC180_YOLO_CATALOGUE_REGISTRY": str(public / "contracts/trust-root-registry-v1.json"),
        "SPEC180_YOLO_CATALOG_DATA_NAME": receipt["catalogueDataName"],
        "SPEC180_YOLO_CATALOG_SIGNER": receipt["catalogueSigner"],
        "SPEC180_YOLO_OFFER_TRUST_ROOT": str(public / "offer-trust-root.json"),
        "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP": str(inputs / "offer-public-key-map.json"),
        "SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP": str(inputs / "offer-private-key-map.json"),
        "SPEC180_YOLO_TOPOLOGY": str(TOPOLOGY_REL),
        "SPEC180_YOLO_CONFIG": str(inputs / "spec183-deploy-config.json"),
        "SPEC181_PROTECTION_EPOCH": "spec183-yolo-protected-v1",
        "NDNSF_SPEC180_CONFIG_ROOT": str(keys.parent),
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "0.01",
    })
    (inputs / "offer-private-key-map.json").write_text(
        json.dumps(private_map, indent=1) + "\n")
    (inputs / "offer-public-key-map.json").write_text(
        json.dumps(host_offer_map, indent=1) + "\n")
    (inputs / "spec183-deploy-config.json").write_text(
        json.dumps(deploy, indent=1) + "\n")

    import subprocess
    command = [sys.executable, str(DRIVER_REL), "--case", args.case]
    print(json.dumps({"status": "T010_START", "case": args.case,
                      "driver": str(DRIVER_REL)}, sort_keys=True))
    completed = subprocess.run(command, env=env, cwd=_REPO_ROOT)
    print(json.dumps({"status": "T010_DONE", "returncode": completed.returncode,
                      "output": str(case_output)}, sort_keys=True))
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
