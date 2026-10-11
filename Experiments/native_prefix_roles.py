"""Actual SVS sync and producer Data strategy choices for native MiniNDN runs."""

import json
from pathlib import Path
import subprocess

from ndn.encoding import Component, Name

BEST_ROUTE = "/localhost/nfd/strategy/best-route"
MULTICAST = "/localhost/nfd/strategy/multicast"


def prefix_role_plan(*, actors, groups, data_roots=(), protocol_version=3):
    if protocol_version not in (2, 3):
        raise ValueError("unsupported configured SVS protocol version")
    roles = {}

    def add(prefix, role, source):
        name = Name.normalize(prefix)
        if not name:
            raise ValueError("prefix role requires a complete non-root Name")
        wire = bytes(Name.encode(name))
        row = dict(prefix=Name.to_str(name), role=role, source=source,
                   required_strategy=MULTICAST if role == "sync" else BEST_ROUTE)
        if wire in roles and roles[wire]["role"] != role:
            raise ValueError("same prefix cannot be Sync and Data")
        roles.setdefault(wire, row)

    for actor in actors:
        add(actor, "data", "configured complete actor; business and node-prefix/G transport")
    for root in data_roots:
        add(root, "data", "configured Data retrieval root")
    for group in groups:
        add(group, "data", "logical group; actual Sync has typed protocol version")
        add(Name.normalize(group) + [Component.from_version(protocol_version)],
            "sync", "selected SVS SyncProtocolCodec::makeSyncName")
    return tuple(sorted(roles.values(), key=lambda row: bytes(Name.encode(Name.from_str(row["prefix"])))))


def configure_prefix_roles(ndn, *, actors, groups, receipt, data_roots=(), protocol_version=3,
                           expected_roles=None):
    plan = prefix_role_plan(actors=actors, groups=groups, data_roots=data_roots,
                            protocol_version=protocol_version)
    if expected_roles is not None and list(plan) != expected_roles:
        raise RuntimeError("SPEC201_CANDIDATE_ACTUAL_PREFIX_ROLES_MISMATCH")
    records = []

    def run(node, arguments):
        from minindn.util import getPopen
        process = getPopen(node, ["nfdc", *arguments], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise RuntimeError(f"nfdc timed out on {node.name}")
        if process.returncode != 0:
            raise RuntimeError(f"nfdc failed on {node.name}: {stderr.decode(errors='replace')}")
        return stdout.decode(errors="strict")

    for node in ndn.net.hosts:
        for row in plan:
            run(node, ["strategy", "set", row["prefix"], row["required_strategy"]])
        text = run(node, ["strategy", "list"])
        table = []
        for line in text.splitlines():
            fields = dict(item.split("=", 1) for item in line.split() if "=" in item)
            if "prefix" in fields and "strategy" in fields:
                table.append((Name.from_str(fields["prefix"]), Name.from_str(fields["strategy"])))
        for prefix, strategy in table:
            owners = [row for row in plan if Name.is_prefix(Name.from_str(row["prefix"]), prefix)]
            if owners:
                owner = max(owners, key=lambda row: len(Name.from_str(row["prefix"])))
                if not Name.is_prefix(Name.from_str(owner["required_strategy"]), strategy):
                    raise RuntimeError(f"conflicting descendant strategy on {node.name}: {Name.to_str(prefix)}")
        effective = []
        for row in plan:
            name = Name.from_str(row["prefix"])
            matches = [(prefix, strategy) for prefix, strategy in table
                       if Name.is_prefix(prefix, name)]
            if not matches:
                raise RuntimeError(f"missing strategy on {node.name}: {row['prefix']}")
            prefix, strategy = max(matches, key=lambda item: len(item[0]))
            if not Name.is_prefix(Name.from_str(row["required_strategy"]), strategy):
                raise RuntimeError(f"wrong effective strategy on {node.name}: {row['prefix']}")
            effective.append(dict(row, matched_prefix=Name.to_str(prefix),
                                  effective_strategy=Name.to_str(strategy)))
        records.append(dict(node=node.name, roles=effective, strategy_list=text,
                            fib_list=run(node, ["fib", "list"])))
    path = Path(receipt)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(dict(protocol_version=protocol_version, nodes=records),
                               indent=2) + "\n")
    return records
