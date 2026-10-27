"""Deployment checks and thin legacy APP command delegation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

def definition_from_json(path):
    from ..app_sdk.contracts import ArtifactReference, DeploymentDefinition

    payload=json.loads(Path(path).read_text())
    return DeploymentDefinition(
        payload["deploymentId"], payload["modelId"],
        tuple(ArtifactReference(**item) for item in payload["artifacts"]),
        tuple(payload["roles"]), payload.get("configuration", {}))


def build_parser():
    parser=argparse.ArgumentParser(prog="ndnsf-di")
    parser.add_argument("--state-root")
    parser.add_argument(
        "--envelope-key-file",
        help="owner-only raw 32-byte key file required for request commands",
    )
    parser.add_argument("--identity", default="operator")
    sub=parser.add_subparsers(dest="command", required=True)
    cluster = sub.add_parser("cluster")
    commands = cluster.add_subparsers(dest="cluster_command", required=True)
    check = commands.add_parser("check", help="check configuration without launching actors")
    check.add_argument("--config", required=True)
    for name in ('up', 'status', 'down'):
        command = commands.add_parser(name)
        command.add_argument('--config', required=True)
        command.add_argument('--timeout', type=float, default=1800,
                             help='bounded operation deadline in seconds (maximum 3600)')
        if name == 'up':
            command.add_argument('--debug', action='store_true',
                                 help='enable NDN diagnostic logs for this boot')
    for name in ("validate","resolve","plan","apply","status","wait","rollback","drain","delete"):
        item=sub.add_parser(name); item.add_argument("target")
        if name in {"apply", "rollback", "drain", "delete"}:
            item.add_argument(
                "--evidence", required=True,
                help="JSON bundle containing trusted Provider public keys and signed evidence")
    request=sub.add_parser("request"); request.add_argument("action", choices=("submit","status","wait","result","cancel","stream")); request.add_argument("target")
    return parser


def load_provider_evidence(path):
    from ..app_sdk.provider import (
        ProviderActionReceipt, ProviderEvidenceVerifier, ProviderReadiness,
    )

    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    if payload.get("schema") != "ndnsf-di-provider-evidence-bundle-v1":
        raise ValueError("unsupported Provider evidence bundle schema")
    keys = {
        str(key_id): str(pem).encode("utf-8")
        for key_id, pem in dict(payload.get("trustedProviderKeys", {})).items()
    }
    verifier = ProviderEvidenceVerifier(keys)
    readiness = tuple(ProviderReadiness.from_dict(item)
                      for item in payload.get("readiness", ()))
    actions = tuple(ProviderActionReceipt.from_dict(item)
                    for item in payload.get("actionReceipts", ()))
    return verifier, readiness, actions


def main(argv=None, *, deployment_factory=None, client_factory=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] == ['_node']:
        from .node import main as run_node
        return run_node(argv[1:])
    parser = build_parser()
    args=parser.parse_args(argv)
    if args.command == "cluster":
        from .deployment_config import DeploymentConfigError, check_deployment

        try:
            if args.cluster_command == 'check':
                result = check_deployment(args.config)
            else:
                from .cluster import run_cluster
                result = run_cluster(args.cluster_command, args.config, args.timeout,
                                     debug=getattr(args, 'debug', False))
        except (DeploymentConfigError, RuntimeError, OSError) as error:
            parser.error(str(error))
        print(json.dumps(result, sort_keys=True))
        return 0 if (args.cluster_command != 'up' or not result.get('failure')) and result.get('status') in {
            'CONFIGURATION_CHECKED', 'COMPLETE', 'STOPPED', 'ABSENT', 'SERVICES_READY'} else 1
    if not args.state_root:
        parser.error("--state-root is required for legacy commands")
    from ..app_sdk.runtime_journal import (
        FileRequestEnvelopeKeyProvider,
        RuntimeJournal,
    )

    if deployment_factory is None:
        from ..app_sdk.deployment import APPDeployment
        deployment_factory = APPDeployment

    key_provider = (
        FileRequestEnvelopeKeyProvider(args.envelope_key_file)
        if args.envelope_key_file else None
    )
    journal=RuntimeJournal(
        args.state_root,
        args.identity,
        envelope_key_provider=key_provider,
    )
    evidence = getattr(args, "evidence", "")
    verifier = None
    readiness = ()
    actions = ()
    if evidence:
        verifier, readiness, actions = load_provider_evidence(evidence)
    deployment=deployment_factory(journal, readiness_verifier=verifier)
    if args.command in {"validate","resolve","plan","apply","rollback"}:
        definition=definition_from_json(args.target)
        if args.command=="validate": result=deployment.validate(definition).digest()
        else:
            if args.command == "rollback":
                result = deployment.rollback(
                    definition, readiness=readiness,
                    activation_receipts=actions).__dict__
                revision = None
            else:
                revision = deployment.resolve(definition)
            result=(deployment.plan(revision).__dict__ if args.command=="plan" else
                    deployment.apply(
                        revision, readiness=readiness,
                        activation_receipts=actions).__dict__
                    if args.command=="apply" else revision.__dict__
                    if revision is not None else result)
    elif args.command in {"status","wait","drain","delete"}:
        if args.command in {"drain", "delete"}:
            result=getattr(deployment,args.command)(
                args.target, action_receipts=actions)
        else:
            result=getattr(deployment,args.command)(args.target)
        result=(result.__dict__ if hasattr(result, "operation_id") else
                getattr(result,"value",result))
    else:
        if client_factory is None:
            from ..app_sdk.client import APPClient
            client_factory = APPClient
        client=client_factory(journal)
        if args.action=="submit": result=client.submit("cli","current",args.target.encode()).__dict__
        else:
            handle=client.open_request(args.target); result=getattr(client,args.action)(handle)
            if hasattr(result,"value"): result=result.value
            elif isinstance(result,bytes): result=result.decode(errors="replace")
            elif args.action=="stream": result=[event.__dict__ for event in result]
    print(json.dumps(result, sort_keys=True, default=str)); return 0


if __name__ == "__main__": raise SystemExit(main())
