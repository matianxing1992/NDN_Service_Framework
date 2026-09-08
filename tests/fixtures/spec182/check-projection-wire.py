#!/usr/bin/env python3
"""Independent offline SDK validation of native-produced dataflow evidence."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'NDNSF-DistributedInference'))
from ndnsf_distributed_inference.sdk.placement import RoleDataflowContract, canonical_bytes

rows = [line for line in Path(sys.argv[1]).read_bytes().splitlines() if line]
assert len(rows) == 7, 'incomplete native projection graph-case evidence'
endpoints = 0
for row in rows:
    parsed = RoleDataflowContract.from_bytes(row)
    assert json.loads(canonical_bytes(parsed)) == json.loads(row)
    endpoints += len(parsed.may_publish) + len(parsed.must_fetch)
    changed = json.loads(row)
    changed['dataflow_digest'] = 'sha256:' + '0' * 64
    try:
        RoleDataflowContract.from_bytes(json.dumps(changed).encode())
    except ValueError:
        pass
    else:
        raise AssertionError('SDK accepted a tampered dataflow digest')
assert endpoints == 11, 'incomplete application/pipeline/redistribution endpoints'
print(json.dumps({'ok': True, 'dataflows': len(rows), 'endpoints': endpoints}))
