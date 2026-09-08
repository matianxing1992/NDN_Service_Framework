"""Spec183 T010: validate per-run inputs for the maintained CPU MiniNDN Y-B driver.

Maps the containerized-issuer products (case.json, offer trust/maps, role
certificates, catalogue names) plus the actual per-run offer private keys
onto the maintained ACK-driven MiniNDN driver
(``Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B``), which runs
the four-role graph over the clean-root host binaries. A transient systemd
service bounds the process tree. Three-case execution, network resource cleanup
evidence and semantic host qualification remain T007 N1/N2 work;
a successful driver exit alone is not a host, GPU or SIF qualification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import site
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
    from jobs.yolo.submit import _load_prepared
    return _load_prepared(output, run_id)


def validated_inputs(output: Path, run_id: str, *, profile_path: Path,
                     preparation_sha256: str) -> dict:
    """Read-only binding check before any host output, process or key-map write.

    The digest is the actual issuer's retained public preparation identity;
    computing it from an arbitrary supplied document here would self-authorize
    that document. This is input integrity only, not a host qualification gate.
    No SIF is opened or required to be newly built by this check.
    """
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ed25519
    from runtime.yolo_profile import load_operator_profile, _read_plane, HASH, application_sync_prefix
    from runtime.yolo_bundle import verify_harness, verify_preparation, _bytes
    from runtime.identities import _read_credential

    prepared = _prepared(output, run_id)
    loaded = load_operator_profile(Path(profile_path), stage='inputs')
    if loaded['documentDigest'] != prepared['profileDigest']:
        raise ValueError('MININDN_PREPARED_PROFILE')
    if not isinstance(preparation_sha256, str) or not HASH.fullmatch(preparation_sha256):
        raise ValueError('MININDN_PREPARATION_DIGEST')
    verify_harness(Path(prepared['bundle']), expected_manifest_sha256=prepared['harnessManifestSha256'])
    root = Path(prepared['plan']['output'])
    public, private = root/'public', root/'private'
    receipt = verify_preparation(public, prepared['plan'],
        expected_receipt_digest=preparation_sha256, candidate_digest=prepared['candidateDigest'])
    profile = loaded['profile']
    ref = profile['workload']['packageManifest']
    manifest = Path(ref['path'])
    wire = _bytes(manifest)
    if (len(wire) != ref['bytes'] or 'sha256:'+hashlib.sha256(wire).hexdigest() != ref['sha256']
            or receipt.get('packageManifestDigest') != ref['sha256']
            or receipt.get('protectionEpoch') != profile['security']['protectionEpoch']):
        raise ValueError('MININDN_PREPARATION_WORKLOAD')
    case = _read_plane(public/'case.json')
    identities = case.get('runtime', {}).get('identities') if isinstance(case, dict) else None
    plan = prepared['plan']
    group = application_sync_prefix(plan.get('applicationName', plan['namespace']))
    if (identities != dict(plan['identities'], group=group) or case.get('group') != group
            or case['runtime'].get('application_name') != plan.get('applicationName', plan['namespace'])):
        raise ValueError('MININDN_PREPARATION_IDENTITIES')

    def key_pair(secret_path, public_path):
        key = serialization.load_pem_private_key(_read_credential(secret_path, private=True),
                                                 password=None, backend=default_backend())
        advertised = serialization.load_pem_public_key(_read_credential(public_path), backend=default_backend())
        if not isinstance(key, ed25519.Ed25519PrivateKey) or not isinstance(advertised, ed25519.Ed25519PublicKey):
            raise ValueError('MININDN_KEY_ALGORITHM')
        def raw(k): return k.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        if raw(key.public_key()) != raw(advertised):
            raise ValueError('MININDN_PRIVATE_KEY_MISMATCH')
        return 'sha256:'+hashlib.sha256(raw(advertised)).hexdigest()

    offer_map = _read_plane(public/'offer-public-key-map.json')
    trust = _read_plane(public/'offer-trust-root.json')
    recipients = _read_plane(public/'recipient-public-keys.json')
    if (not isinstance(offer_map, dict) or len(offer_map) != len(ROLES)
            or not isinstance(recipients, dict) or set(recipients) != {identities[r] for r in ROLES}
            or not isinstance(trust, dict) or trust.get('candidateId') != receipt.get('placementCandidateId')
            or trust.get('candidateDigest') != receipt.get('placementCandidateDigest')
            or not isinstance(trust.get('entries'), list) or len(trust['entries']) != len(ROLES)):
        raise ValueError('MININDN_PREPARATION_KEY_MAP')
    private_map, host_offer_map, recipient_map = {}, {}, {}
    for role in ROLES:
        identity = identities[role]
        secret = private/role/'offer.pem'
        key_id = key_pair(secret, public/'offers'/(role+'.pub'))
        if offer_map.get(key_id) != '/config/offers/'+role+'.pub':
            raise ValueError('MININDN_OFFER_MAP_BINDING')
        matching = [row for row in trust['entries'] if isinstance(row, dict) and row.get('provider') == identity]
        if len(matching) != 1 or matching[0].get('signerKeyId') != key_id:
            raise ValueError('MININDN_OFFER_IDENTITY_BINDING')
        private_map[identity] = str(secret)
        host_offer_map[key_id] = str(public/'offers'/(role+'.pub'))
        relative = 'recipients/'+role+'.pub'
        row = recipients[identity]
        if (not isinstance(row, dict) or set(row) != {'path','sha256'} or row['path'] != relative
                or row['sha256'] != 'sha256:'+hashlib.sha256(_read_credential(public/relative)).hexdigest()):
            raise ValueError('MININDN_RECIPIENT_MAP_BINDING')
        key_pair(private/role/'recipient.pem', public/relative)
        recipient_map[identity] = str(private/role/'recipient.pem')
    authority = private/'user/authority/artifact-policy-authority.key'
    key_pair(authority, public/'contracts/authority.pub')
    for name in ('request-envelope.key', 'requester.key'):
        if len(_read_credential(private/'user'/name, private=True)) != 32:
            raise ValueError('MININDN_USER_KEY')
    return dict(prepared=prepared, profile=profile, receipt=receipt, case=case,
                public=public, private=private, package=manifest.parent,
                privateMap=private_map, offerMap=host_offer_map, recipientMap=recipient_map)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", type=Path, default=PROFILE_REL)
    parser.add_argument("--preparation-sha256", required=True,
                        help="Retained issuer digest of public/preparation.json")
    parser.add_argument("--case", choices=("Y-B",), default="Y-B")
    parser.add_argument("--library-path", default="/tmp/t008-build-root/lib")
    args = parser.parse_args(argv)

    output = Path(os.path.abspath(str(args.output)))
    run_root = output / args.run_id
    public = run_root / "public"
    private = run_root / "private"
    checked = validated_inputs(output, args.run_id, profile_path=args.profile,
                               preparation_sha256=args.preparation_sha256)
    prepared, receipt, case = (checked[key] for key in ('prepared','receipt','case'))
    identities = case["runtime"]["identities"]

    private_map, host_offer_map = checked['privateMap'], checked['offerMap']
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

    host_root = run_root / 'host-minindn'
    host_root.mkdir(mode=0o700, exist_ok=False)
    state, case_output, inputs = (host_root/name for name in ('state', 'output', 'inputs'))
    for directory in (state, inputs):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    # The driver demands an exclusive, initially empty output root.
    case_output.mkdir(mode=0o700, parents=True, exist_ok=False)

    # Pass only the declared host inputs and executable/module search paths.
    # Inherited SIF bypass knobs or unrelated credentials are not host inputs.
    env = {name: os.environ[name] for name in ('PATH', 'PYTHONPATH') if name in os.environ}
    # The systemd system manager executes the outer MiniNDN driver as root,
    # whose Python user-site is different from the operator's.  The driver
    # performs a read-only ONNX/catalogue validation before starting NFD; keep
    # that dependency path explicit across the privilege boundary.  SIF child
    # commands replace PYTHONPATH with the image-owned path in `sif_exec_prefix`.
    operator_site = Path(site.getusersitepackages())
    if operator_site.is_dir():
        env['PYTHONPATH'] = ':'.join(filter(None, (str(operator_site), env.get('PYTHONPATH', ''))))
    # In exact-SIF mode the outer validator is still the host Python process;
    # do not inject a stale developer build root that can shadow its matching
    # system ABI.  Every NFD/application child receives the image-owned
    # LD_LIBRARY_PATH from `sif_exec_prefix`.
    host_library_path = "" if os.environ.get("SPEC180_RUNTIME_SIF", "").strip() else args.library_path
    env.update({
        "PYTHONDONTWRITEBYTECODE": "1",
        "LD_LIBRARY_PATH": host_library_path,
        "NDNSF_DI_STATE_ROOT": str(state),
        # The systemd system manager runs the MiniNDN owner as root so it can
        # create network namespaces.  The child must still bind its state
        # directory to the unprivileged operator who created this run.
        "NDNSF_DI_STATE_ROOT_OWNER_UID": str(state.stat().st_uid),
        "NDNSF_DI_ENVELOPE_KEY_FILE": str(private / "user/request-envelope.key"),
        "SPEC180_CASE_OUTPUT_DIR": str(case_output),
        "SPEC180_YOLO_CANONICAL_PACKAGE": str(checked['package']),
        "SPEC180_YOLO_CATALOGUE_REGISTRY": str(public / "contracts/trust-root-registry-v1.json"),
        "SPEC180_YOLO_CATALOG_DATA_NAME": receipt["catalogueDataName"],
        "SPEC180_YOLO_CATALOG_SIGNER": receipt["catalogueSigner"],
        "SPEC180_YOLO_OFFER_TRUST_ROOT": str(public / "offer-trust-root.json"),
        "SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP": str(inputs / "offer-public-key-map.json"),
        "SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP": str(inputs / "offer-private-key-map.json"),
        "SPEC180_YOLO_TOPOLOGY": str(TOPOLOGY_REL),
        "SPEC180_YOLO_CONFIG": str(inputs / "spec183-deploy-config.json"),
        "SPEC181_PROTECTION_EPOCH": receipt['protectionEpoch'],
        "NDNSF_SPEC180_CONFIG_ROOT": str(private/'user/authority'),
        "SPEC181_REQUESTER_PRIVATE_KEY": str(private/'user/requester.key'),
        "SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(public/'contracts/authority.pub'),
        "NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP": str(public/'recipient-public-keys.json'),
        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(inputs/'recipient-private-key-map.json'),
        "NDNSF_TIMELINE_TRACE_SAMPLE_RATE": "0.01",
    })
    # Preserve the selected exact-SIF command provider across the systemd
    # boundary.  Without these three values the outer driver falls back to
    # the host-source freshness gate and never reaches the sealed children.
    for name in ("SPEC180_RUNTIME_SIF", "SPEC180_RUNTIME_APPTAINER",
                 "SPEC180_RUNTIME_APP_ROOT"):
        value = os.environ.get(name, "").strip()
        if value:
            env[name] = value
    from runtime.identities import _credential_document
    for name, value in {'offer-private-key-map.json': private_map,
                        'offer-public-key-map.json': host_offer_map,
                        'recipient-private-key-map.json': checked['recipientMap'],
                        'spec183-deploy-config.json': deploy}.items():
        _credential_document(inputs/name, value)

    from runtime.host_minindn import supervise
    command = [sys.executable, str(DRIVER_REL), "--case", args.case]
    print(json.dumps({"status": "T010_START", "case": args.case,
                      "driver": str(DRIVER_REL)}, sort_keys=True))
    timing = checked['profile']['timing']
    seconds = (timing['stagingSeconds'] + timing['startupSeconds']
               + (timing['requestDeadlineMs'] + 999) // 1000)
    returncode = supervise(command, env, host_root/'supervisor', cwd=_REPO_ROOT,
                          seconds=seconds, cleanup_seconds=timing['cleanupSeconds'])
    print(json.dumps({"status": "T010_DONE", "returncode": returncode,
                      "output": str(case_output), "qualification": "NOT_EVALUATED"}, sort_keys=True))
    return returncode


if __name__ == "__main__":
    sys.exit(main())
