#!/usr/bin/env python3
import argparse
import json
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--facts")
parser.add_argument("--account-root")
parser.add_argument("--project-root")
parser.add_argument("--scratch-root")
parser.add_argument("--evidence-root")
parser.add_argument("--minimum-free-bytes", type=int, required=True)
args = parser.parse_args()
if args.facts:
    value = json.load(open(args.facts, encoding="utf-8"))
else:
    import os
    if not all((args.account_root,args.project_root,args.scratch_root,args.evidence_root)):
        parser.error("live mode requires all storage roots")
    paths={"accountRoot":args.account_root,"projectRoot":args.project_root,"scratchRoot":args.scratch_root,"evidenceRoot":args.evidence_root}
    filesystems={}
    for path in paths.values():
        probe=path
        while not os.path.exists(probe): probe=os.path.dirname(probe)
        stat=os.statvfs(probe); filesystems[path]={"freeBytes":stat.f_bavail*stat.f_frsize,"sharedCapacityBytes":stat.f_blocks*stat.f_frsize}
    value={"paths":paths,"filesystems":filesystems,"quota":{"source":"unavailable","verifiedByCommand":False}}
paths = value["paths"]
filesystems = value.get("filesystems", {})
quota = value.get("quota", {})

def fail(code):
    print(code, file=sys.stderr)
    raise SystemExit(3)

if (not paths["projectRoot"].startswith("/project/") or
        not paths["evidenceRoot"].startswith(paths["projectRoot"] + "/evidence/") or
        not paths["scratchRoot"].startswith("/tmp/ndnsf-di-")):
    fail("STORAGE_PATH_POLICY_INVALID")
if quota.get("verifiedByCommand") and quota.get("remainingBytes", 1) < args.minimum_free_bytes:
    fail("STORAGE_QUOTA_EXHAUSTED")
for key in ("projectRoot", "scratchRoot"):
    if filesystems.get(paths[key], {}).get("freeBytes", 0) < args.minimum_free_bytes:
        fail("STORAGE_SPACE_INSUFFICIENT:" + key)
print(json.dumps({"status": "PASS", "paths": paths, "filesystems": filesystems, "quota": quota}, sort_keys=True))
